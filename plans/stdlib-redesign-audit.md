# DMM standard library: audit and replacement design

**Status:** review draft; audit and design only. No implementation is authorized by this document.

**Snapshot:** current working tree, audited 2026-10-07; HEAD `5d5f916`. Existing uncommitted work is part of the audited implementation. Recommendations intentionally allow breaking changes.

## 1. Executive summary

DMM already has substantially more useful library machinery than its public organization suggests. It has owning collections, checked borrow propagation, move-only values, structural generics, explicit error propagation, portable filesystem operations, Unicode processing, an executor implemented in DMM, and a portable asynchronous networking engine. These are valuable foundations. A replacement should consolidate them rather than discard capability to achieve a small API.

The most serious complexity is concentrated in a few places:

* Root `stdlib`, `stdlib/stdio`, and `stdlib/fs` overlap on file and stream operations. Their error and ownership conventions disagree.
* `stdio.Stream`, `stdio.Bytes`, and buffered adapters carry owning resources in Copy-shaped/manual-release APIs. Root collection owners use destructors. Ordinary application code should have one resource model.
* Byte storage is implemented more than once. Root `Bytes` and `List<u8>` largely duplicate growth and ownership behavior; `stdio.Bytes` adds a third convention.
* `core` mixes canonical type properties and memory mechanisms with printing, process exit, atomics, shared ownership, the executor, and networking.
* Allocation errors are both enum status values and Result payloads; some constructors produce failed objects with sticky errors. This makes otherwise simple calls require repeated validity checks.
* Hash collections split Copy and borrowed callbacks into multiple adapters and entry points. The distinction is useful internally, but should not dictate the main public interface.
* Some public APIs expose implementation details: descriptor numbers, cursor operations that repeatedly take their buffer, numeric Unicode category codes, and runtime entry points.

The library is also small in the wrong places. Its owning collections do not yet implement the new iteration protocol. It lacks portable CLI arguments and process execution, coherent high-level Reader/Writer interoperability, a practical owned UTF-8 text model with length-based borrowed views, and a consistent application-level networking package.

The proposed replacement has:

1. A deliberately small conceptual core: Option/Result/propagation, canonical type properties, an iterator contract, and an explicit memory substrate.
2. One owning sequence implementation, `List<T>`, plus genuinely different `Buffer<T>`, `String`, `Map<K,V>`, and `Set<T>`.
3. One synchronous I/O model using checked references, destructors, Result, and meaningful Reader/Writer interfaces.
4. Portable `fs`, `net`, `process`, `time`, `async`, and `sync` packages.
5. Explicit raw/native packages, with compiler-facing implementation packages hidden from ordinary users.
6. Compact iteration with explicit shared/mutable modes and a small convenience layer, without a Rust-sized combinator hierarchy.

This is a consolidation and capability plan, not a language redesign. Some proposed operations need new DMM FFI bindings or runtime plumbing. Those dependencies are identified below.

### Evidence and scope

The audit covers the hand-written standard library, its READMEs, generated Unicode table structure and generator, `LANGUAGE_SPEC.md`, `ARCHITECTURE.md`, `AGENTS.md`, stdlib/iterator/ownership/async tests, and application examples. It also examines compiler semantic handling, package loading, runtime ABI headers, and platform-selected implementations. The generated Unicode data was examined through its generator, table organization, pinned version, and conformance inputs; this audit does not independently revalidate every generated table entry.

`FORMAL_LANGUAGE.md` was not found in the repository or its parent project. It was therefore not a source of assumptions. The implemented language and current specification take precedence over older comments and examples.

Important source anchors:

| Area | Sources |
|---|---|
| Language and ownership | [LANGUAGE_SPEC.md](../LANGUAGE_SPEC.md), [ARCHITECTURE.md](../ARCHITECTURE.md), [AGENTS.md](../AGENTS.md) |
| Root foundations | [foundation.dmm](../stdlib/foundation.dmm), [types.dmm](../stdlib/types/types.dmm), [functional.dmm](../stdlib/functional.dmm) |
| Owning sequences and hash collections | [collections.dmm](../stdlib/collections.dmm), [collections/collections.dmm](../stdlib/collections/collections.dmm) |
| I/O overlap | [io.dmm](../stdlib/io.dmm), [stdio](../stdlib/stdio/README.md), [fs](../stdlib/fs/README.md) |
| Text and Unicode | [text.dmm](../stdlib/text/text.dmm), [unicode.dmm](../stdlib/unicode/unicode.dmm), [generator](../stdlib/unicode/generate.py) |
| Async and networking | [async](../stdlib/async/README.md), [executor](../stdlib/core/executor/README.md), [networking](../stdlib/core/net/README.md) |
| Other portable facilities | [time](../stdlib/time/README.md), [sync](../stdlib/sync/README.md), [memory](../stdlib/memory/README.md), [native](../stdlib/native/README.md) |
| Application integration | [examples](../examples/README.md), [stdlib contracts](../tests/stdlib/README.md), [language gaps](stdlib-language-gaps.md) |

### Implementation feasibility labels

* **L — library:** expressible using current DMM semantics and existing mechanisms.
* **B — binding:** needs compiler/package/runtime binding changes, without adding a language feature.
* **R — runtime/platform:** needs new platform bindings or startup/runtime plumbing.
* **V — verification gate:** borrow propagation, overload selection, or generic instantiation must be proven in a focused contract test before committing the final API.

These labels are dependencies, not permission to implement during this audit.

## 2. Current stdlib inventory

### Packages and meaningful public families

| Current package | Public families and implementation role |
|---|---|
| `stdlib` | Option/Result reexports; propagation interfaces; functional helpers; Cell/Printable/Equal; scalar print/println; legacy Input/System/raw file wrappers; owning Bytes, Buffer, List, String |
| `stdlib/types` | Option, Result, Propagation, NoneResidual, AllocationError including success and invalid-index states |
| `stdlib/core` | Canonical Copy/Send/Sync; Poll; allocation and lifetime primitives; memory forwarding; atomics; Shared; raw I/O and process helpers |
| `stdlib/core/raw` | Allocation, byte memory, pointer/string operations, descriptor I/O, exit/trap; overlaps root core forwarding |
| `stdlib/core/executor` | DMM worker queues, wake/retain/release machinery, private compiler-facing scheduler ABI, platform threading |
| `stdlib/core/net` | Address and endpoint types; socket ownership; resolve, connect, bind/listen/accept, read/write, UDP, deadlines, cancellation |
| `stdlib/core/net/internal` | Address layouts, operation ownership, epoll/IOCP, DNS workers, completion acknowledgment, instrumentation |
| `stdlib/collections` | Deque; HashMap with Copy/borrowed callback policies; HashSet restricted to Copy elements |
| `stdlib/algorithms` | Searching, predicates, folds, equality, reverse/sort, binary search; Copy and borrowed callback families; mutation/copy/fill/transform |
| `stdlib/memory` | Box and byte Arena |
| `stdlib/text` | Borrowed byte operations, ASCII trim, byte-delimited Split, StringBuilder, byte clone/replace helpers |
| `stdlib/unicode` | UTF-8 decoding/encoding/validation, UTF-16 conversion, scalar traversal, eager codepoints, properties, casing, normalization; pinned Unicode 17 tables |
| `stdlib/numeric` | Checked signed/unsigned 64-bit arithmetic, base parsing, caller-buffer formatting, numeric errors and duplicate limits |
| `stdlib/limits` | Primitive numeric bounds |
| `stdlib/math` | Floating-point functions, classification, min/max/clamp, constants; platform libm/UCRT |
| `stdlib/binary` | Endian integer reads/writes, position cursors, checked borrowed reader/writer adapters |
| `stdlib/stdio` | Copy/manual-close Stream, transfer/status types, manual-release Bytes, buffered adapters, text/line helpers, copy and duplicate file helpers |
| `stdlib/fs` | RAII File, open modes, read/write/seek, whole-file operations, metadata, namespace changes, Directory and entries |
| `stdlib/path` | Lexical path operations with platform-selected behavior |
| `stdlib/env` | Owned byte environment reads, set/remove; Linux getenv and Windows wide-character bindings |
| `stdlib/time` | Duration, Instant, SystemTime, clock queries, checked duration arithmetic, blocking sleep |
| `stdlib/random` | Deterministic seeded generator, unbiased bounded values, OS entropy |
| `stdlib/sync` | Send/Sync-aware Mutex and non-Send guard |
| `stdlib/async` | Waker/Context/Poller, poll adapters, join2/select2/race2, void adapters, timers, sleep/timeout |
| `stdlib/native/*` | glibc, pthreads, kernel32, UCRT, Winsock ABI declarations; private threading bridge |
| `stdlib/system` | Automatically loaded source-runtime provider, not an ordinary application abstraction |

Unicode tables are large because of data coverage, not because of a giant public framework. That size alone is not bloat.

### Current dependency shape

~~~text
root stdlib ──> types + core
collections / algorithms / memory / text / numeric / binary ──> root + core
stdio ──> core/raw + root storage/error conventions
fs ──> root/core + time + platform bindings (+ UTF conversions on Windows)
env/path/time/random/math ──> platform-specific implementations
async ──> core Poll/atomics + executor ABI + threading
core/net ──> executor + net/internal + native bindings
sync ──> core properties + sync/internal + native bindings
system provider ──> compiler-required source runtime components
~~~ 

Consequently, changing the root façade can affect almost every package. Moving runtime directories also affects compiler retention and provider logic; it is not merely a filesystem cleanup.

### Language constraints that govern the replacement

* Imports are slash paths. Public imports forward declarations; there are no ordinary type aliases or selective public imports. Nominal identity must be preserved deliberately.
* Structural interfaces and generic bounds exist. Associated types, interface inheritance, default interface methods, primitive extension methods, higher-ranked lifetime contracts, and async interface methods do not.
* Checked references have origin tracking. Stored checked references need explicit lifetime parameters. Returned borrows must originate in parameters, receiver, or package storage.
* Checked-reference auto-deref works for member access, indexing, and method resolution. It does not imply argument conversion, arithmetic dereference, raw-pointer dereference, or a new auto-borrow rule.
* Methods have an implicit receiver. Receiver mutability/consumption contracts matter; `self` is an addressable receiver, not an additional ABI parameter. A method cannot arbitrarily move or replace its whole receiver.
* Whole owned enum matches can consume payloads. Generic `void` is not an ordinary field/payload type; existing Result/Future void handling does not establish a universal Unit conversion.
* Primitive `string` is NUL-terminated borrowed storage, not a length-based RAII owner. Raw slices are nonowning descriptors; a returned `u8[]` is not itself an immutable view.
* Resources with destructors and mutable references are move-only where required. Raw heap element movement can use existing initialize/take/destroy machinery; arbitrary partial moves from tracked aggregates must not be assumed.
* Async is feature-gated in the manifest. Future, JoinHandle, and Executor obligations include completion/cancellation consumption. Cancellation retains storage until acknowledgment.
* Spawn checks concrete future captures for Send; `Future<T>` with Send output is not by itself sufficient. A future borrowing caller storage is not an independent spawn candidate.
* Package initialization follows dependencies; package cleanup is reversed. Immediate exit bypasses ordinary cleanup. Avoid resource-owning global initialization for convenience.
* Native FFI accepts ABI-safe scalar/pointer/native aggregate shapes, not arbitrary DMM strings, enums, slices, references, or futures.
* Current practical targets are x86-64 Linux/glibc and Windows/MinGW-w64 UCRT64. Platform suffix selection occurs before parsing.

## 3. KEEP / SIMPLIFY / MERGE / MOVE / RENAME / REMOVE analysis

The table groups meaningful families rather than listing every width-specific overload.

| Existing family | Decision | Replacement and reason |
|---|---|---|
| Option, Result, propagation types and interfaces | KEEP, MOVE, SIMPLIFY | Canonical core. Option defaults to None; Result has no fabricated default success. Keep consuming propagation and borrowed matching. |
| Option/Result map, andThen, mapError, lazy fallback | KEEP, MOVE | Core free functions with once callbacks. They provide reusable ownership-correct composition. |
| unwrapOr versus lazy optionOr | SIMPLIFY | Keep eager `unwrapOr` and explicitly named lazy `unwrapOrElse`; these differ in evaluation/ownership, not spelling alone. |
| Cell with public value/get/set | REMOVE | Ordinary variable/struct field already supplies this behavior; no interior-mutability invariant exists. |
| Printable returning primitive string; printValue by value | REMOVE | Does not settle allocation or string lifetime and can consume an owner just to print. Use scalar/text output; user formatting remains explicit. |
| Equal with by-value Self | REMOVE | Incompatible with general move-only comparisons. Use borrowed callbacks where an algorithm actually needs equality. |
| Root print/println scalar overloads | MOVE, SIMPLIFY | `io.print/println`, return Result, share encoding implementation. Width overloads are justified by distinct primitive types. |
| Public print_fixed helpers | REMOVE | Private formatter implementation; no separate public contract. |
| Input.init/readInt/readChar/readString | REMOVE | Checked Reader + line reading + numeric parsing. No initialization ceremony or scanf-like unchecked convention. |
| System descriptor enum and root raw file wrappers | REMOVE, MOVE | Portable I/O uses resources. Explicit raw descriptors live in `io/raw`. |
| Root Bytes and stdio.Bytes | MERGE | `collections.List<u8>`, one allocator/destructor/growth/error model. No compatibility Bytes alias. |
| Buffer<T> | KEEP, MOVE, SIMPLIFY | Fixed-length owning storage is a real invariant. Explicit initialization; no generic zero fabrication. |
| List<T> | KEEP, MOVE, RENAME | `collections.List<T>`, append/reserve/get/getMut/pop/remove, Result errors, iterator modes. |
| Sticky collection constructor errors and ok/error accessors | REMOVE | Constructors return Result or produce a valid allocation-free empty object. |
| getCopy/viewCopy duplicates | SIMPLIFY | Borrowed get/view first; explicitly dereference Copy values. Keep special copying only where it adds a distinct operation. |
| List bulk append snapshotting | SIMPLIFY | Bulk Copy append specifies disjointness and borrow rules. Do not force an allocation for every append to accommodate aliasing raw views. |
| Root String clone/view conventions | REPLACE, MOVE | Owned length-based validated UTF-8 String plus checked Text view. Primitive string remains visibly separate. |
| Deque | REMOVE from stdlib | Production consumers inspected do not require this public structure; the executor has a specialized private queue. External package can preserve it. |
| HashMap | KEEP, RENAME, SIMPLIFY | Map; borrowed keys and values; one callback contract; no dynamically allocated policy interface merely to store hash/equality functions. |
| HashSet<T:Copy> | KEEP, RENAME, SIMPLIFY | Set<T> uses the same borrowed key policy and supports owned/move-only keys. |
| CopyCallbacks/BorrowedCallbacks/KeyCallbacks adapter hierarchy | MERGE, REMOVE | Static function fields with borrowed keys; built-in primitive/text policies use compile-time specialization. |
| Copy/borrowed algorithm twins | MERGE | Borrowed predicate contracts for general algorithms; consuming iterator operations for produced values. Retain distinct Copy operations only when copying is intrinsic. |
| sort/reverse/binary search | KEEP, SIMPLIFY | Slice-specific algorithms remain; they are not replaced by a combinator ecosystem. Mutating APIs require checked mutable slice access. |
| Box | KEEP, SIMPLIFY | RAII owner; borrowed get/getMut; consuming free-function extraction avoids an emptied live Box. |
| Arena | REMOVE from initial stdlib | Current byte-offset arena is not integrated with typed destruction or another production requirement. External allocation package, not foundational core. |
| Text search/trim/split | KEEP, SIMPLIFY | String/Text methods and explicit byte algorithms. Split acquires a real loop factory. |
| StringBuilder | KEEP, SIMPLIFY | Valid UTF-8 builder, Result operations, explicit consuming finish. It adds amortized construction, not another general byte buffer. |
| UTF-8/UTF-16 codec operations | KEEP, MOVE | Small `text/utf8` and `text/utf16` layers; OS conversion does not pull in all Unicode tables. |
| Unicode properties/casing/normalization | KEEP, SIMPLIFY | Explicit `unicode` package; typed category, well-defined scalar/text contracts, retain pinned tests. |
| Numeric categoryCode | RENAME | `category` returns Category, an invariant-bearing enum instead of an undocumented numeric code. |
| Eager codepoints versus Codepoints cursor | SIMPLIFY | Cursor is primary; named `collectCodepoints` makes allocation explicit. Validated text cursor is infallible. |
| Numeric checked arithmetic/base parsing/caller-buffer formatting | KEEP | `numeric`; clear invalid input/overflow/capacity errors, no implicit allocation. |
| Duplicate numeric limit constants | MERGE | `limits` is canonical; remove numeric aliases. |
| Primitive bit bounds | REMOVE | True/false are already the complete meaningful API. |
| Floating math and meaningful constants | KEEP | No broad numeric trait hierarchy. Root min/max/clamp use supported concrete overloads. |
| Binary stateless cursor and borrowed wrappers | MERGE | One checked borrowed Reader/Writer carrying its buffer and cursor. Stateless encoding helpers remain only where independently useful. |
| stdio Stream/status/Transfer/ByteResult hierarchy | REPLACE | RAII resources + Reader/Writer + Result; partial progress carried in a meaningful I/O error. |
| stdio buffered raw-pointer adapters | REPLACE | Checked lifetime-bound BufferedReader/Writer; exclusive borrowed stream and automatic memory cleanup. |
| Three readFile/writeFile families | MERGE | `fs.readFile/writeFile`; generic stream helpers in `io`. |
| fs File and seek | KEEP, SIMPLIFY | Preserve resource lifecycle, add generic I/O interoperability, conventional open/create/openAppend. |
| fs Directory end-as-error | SIMPLIFY | `next()->Result<Option<Entry>,fs.Error>`; eager `readDir` convenience. |
| fs errors | SIMPLIFY | Normalize portable kinds including NotFound/AlreadyExists; preserve native codes; allocation failure remains explicit. |
| Path lexical operations | KEEP, SIMPLIFY | Ordinary paths stay strings/byte inputs; no Path/PathBuf/OsString hierarchy without need. |
| Environment operations | MOVE, SIMPLIFY | `process`: missing variable is Option, not exceptional failure; raw byte forms are explicit. |
| Duration/Instant/SystemTime | KEEP | Units and clock domains justify these types. Checked overflow and clock errors stay visible. |
| Net deadline representation | MERGE | `time.Deadline` shared by networking/timers; no second clock convention. |
| Random generator and OS entropy | KEEP | Explicit deterministic noncrypto generator versus OS entropy; no crypto framework. |
| Shared | MOVE | `memory.Shared<T>`, not canonical type-system core. Preserve Send/Sync derivation. |
| AtomicBit/AtomicUsize | MOVE | Explicit `sync/atomic`; preserve existing SeqCst contract. |
| Mutex and non-Send guard | KEEP | Useful synchronization with genuine borrowing and cleanup invariants. |
| Future/executor/task/join/cancellation builtins | KEEP | Do not create a second task model or rename compiler syntax as a library-only change. |
| Poll/Context/Waker/Poller/manual polling | MOVE, SIMPLIFY | Advanced `async/poll`; keep acknowledgment semantics and refresh/retention rules. |
| join2/select2/race2 | RENAME | join/select/race; two-input operations initially. Their ownership differences remain explicit. |
| Void helpers/asUnit | SIMPLIFY, KEEP where needed | Direct void overloads where valid; Unit adapter for mixed composition. No imaginary generic void storage. |
| Public low-level Timer | REMOVE from normal API | async sleep/timeout suffice; timer polling state stays internal/advanced only if an actual embedding use requires it. |
| Timer worker per timer | SIMPLIFY internally | Share runtime deadline scheduling where feasible; no public timer manager framework. |
| core/net public networking | MOVE, SIMPLIFY | `net`, protocol-specific resource types and portable operations over the same reactor. |
| Raw socket/handle exposure | MOVE | Deliberate `net/raw` or native packages; no portable façade dependency on native handle layout. |
| glibc/pthreads/kernel32/UCRT/Winsock | KEEP, MOVE private parts | Explicit ABI escape hatch; private threading/runtime exports become internal components. |
| system provider and __dmm exports | MOVE internally, KEEP mechanism | Compiler/runtime contract must be retargeted and tested, not removed because its symbol count is large. |

### Representative before/after

Collection failure becomes a single operation-level contract:

~~~dmm
// Current:
var values = stdlib.list<int>(8);
if (!values.ok()) { return 1; }
if (values.push(42) != stdlib.AllocationError.None) { return 2; }

// Target, inside a Result-returning function:
var values = collections.list<int>();
values.reserve(8)?;
values.append(42)?;
~~~

Whole-file reads stop choosing between three storage/error systems:

~~~dmm
// Target:
var bytes = fs.readFile("settings.txt", 1048576)?;
var storage = bytes.view();
var content = text.view(&storage)?;
~~~

The view descriptor must retain the owner origin in compiler analysis. This is already the model used by checked descriptor adapters; it is a V gate, not a claim that raw slices enforce immutability themselves.

Hash operations stop selecting copy/borrowed method names:

~~~dmm
var users = collections.map<int, int>()?;
users.insert(7, 12)?;
var key: int = 7;
match (users.get(&key)) {
    Some(value) => io.println(*value)?;
    None => {}
}
~~~

The extra checked borrow is intentional; ordinary arguments do not auto-borrow.

## 4. Missing functionality

### Essential for the replacement

| ADD | Why standard-library functionality | Dependency |
|---|---|---|
| Collection iter/iterRef/iterMut factories | Containers must work with the language's actual loop protocol; users should not reinvent ownership-sensitive cursors | L/V |
| Generic Reader/Writer and consistent helpers | Files, standard streams, memory and adapters need one common short-read/write contract | L/V |
| Owned length-based UTF-8 text and checked borrowed Text | Search/slicing/embedded NUL/borrowing cannot be coherently represented by NUL-only primitive string | L/V |
| Portable stdin/stdout/stderr resources | Necessary for every CLI; distinguish borrowed standard streams from owned files | L/R |
| CLI args and normal exit-status handling | Basic application entry support; parameterless main currently has no argv API | R/B |
| Process environment and cwd | Common utilities; consolidate existing env and add cwd bindings | L/R |
| Portable process run | Common CLI/system utility requirement; implement with DMM FFI, not a shell-command string | R |
| Portable net façade and text host/address conversion | Networking already exists but lives under core; ordinary clients/servers need discoverable operations | L/V |
| Standard Map/Set policies | Existing callback machinery needs practical defaults for scalar and text keys | L/V |
| Unified allocation/result errors and UTF-8 validation boundary | Required to eliminate failed-object states and unsafe conversion ambiguity | L/V |
| Basic path and filesystem conveniences | Directory listing, recursive directory creation, existence checks with real errors preserved | L/R |
| Strict decimal floating parsing/formatting | Routine text processing should not require scanf or locale-sensitive ad hoc conversion | L/R; reusable DMM codec preferred |

`exists` must distinguish absence from permission/I/O failure: `Result<bit,fs.Error>`. Recursive directory creation is broadly reusable and removes repetitive, race-sensitive application code.

### Useful, after the first coherent surface

* Bounded async channel for owned Send messages, with explicit close and cancellation semantics. This connects existing tasks without a concurrency framework.
* Slice-specific byte search and memory Reader/Writer implementations. They support parsers, tests, and protocol code.
* Explicit UTF-8 line reading and validation helpers on buffered I/O.
* DNS/address formatting and UDP conveniences over existing networking capabilities.
* Map/Set borrowed iteration; stable specified mutation rules rather than a promise of stable iteration order.

### Optional/later, with a concrete admission gate

* Nonblocking Mutex tryLock only if a real consumer needs it.
* Independent Child spawning and pipe redirection after a complete wait/reap/cancel/drop contract is reviewed. Initial `process.run` is synchronous and structured.
* Lazy map/filter wrappers only after concrete callable storage plus lifetime propagation is demonstrated without dynamic erasure or higher-ranked contracts.
* Seekable buffered adapters only if required by actual parsers; avoid prematurely combining all reader/seek traits.

No public thread layer is needed for the initial replacement: the existing executor, Send/Sync, Shared, and Mutex already support concurrent programs. Raw native threads remain available deliberately. No linked lists, heaps, B-trees, calendars, timezone database, HTTP framework, or comprehensive crypto library is proposed.

## 5. Proposed core

### Public root core

Exactly these families belong in conceptual core:

1. **Copy, Send, Sync.** Compiler-canonical properties used across ownership and concurrency.
2. **Option<T>, Result<T,E>.** Fundamental absence and fallibility contracts.
3. **Propagation<T,R>, NoneResidual, Propagate<T,R>, FromResidual<R>.** Existing `?` protocol; implementations require the shared identities.
4. **AllocError { OutOfMemory, CapacityOverflow }.** Shared allocation failure contract for containers and memory owners. Success is represented by Result.Ok, never an error variant called None.
5. **Iterator<T>.** Minimal structural `next()->Option<T>` contract for generic consuming operations.
6. **Small Option/Result consuming helpers.** mapOption, andThenOption, mapResult, mapError, andThenResult, unwrapOr, unwrapOrElse. Once callbacks preserve owner transfer.

Do not put Reader/Writer, Poll, Shared, atomics, I/O, timers, sockets, executors, Unicode, ordinary containers, numeric abstractions, or generic comparison/formatting interfaces here.

### Explicit core memory substrate

`core/memory` contains only allocator/lifetime primitives necessary to implement generic owning storage: allocate/release, initialize/destroy, and their explicit raw pointer contracts. Its allocation source must remain compatible with the current generated allocator. Checked owning abstractions live in normal `memory`.

This is not an invitation to move every low-level operation into core. Bulk byte manipulation, raw pointer helpers such as null/offset, native allocation, descriptors, OS handles and ABI bindings belong in deliberate low-level packages.

### Physical package identity

Initially use `"stdlib/core"` as the physical core import, with `"stdlib/core/memory"` as the explicit substrate. `core` is the conceptual layer and default binding name.

The compiler currently recognizes canonical properties via `stdlib/core`, and iterator validation resolves `stdlib.Option`. Root `stdlib` must therefore publicly forward the canonical core Option declaration until resolver logic is deliberately changed. It should not forward the entire normal library.

A later standalone `"core"` module is reasonable, but requires B work: canonical role lookup, iterator Option resolution, package/provider loading, runtime component retention, and all relevant contract tests. Pretending the current compiler supports arbitrary canonical identities would make this design misleading.

## 6. Proposed stdlib package structure

~~~text
stdlib                         # core public forwarding + min/max/clamp only
├── core                       # conceptual core, compiler canonical identity
│   └── memory                 # essential explicit allocator/lifetime substrate
├── collections                # List, Buffer, Map, Set
├── iter                       # compact convenience operations
├── algorithms                 # slice sort/search/copy/fill/reverse
├── text                       # String, Text, StringBuilder; basic text operations
│   ├── utf8                   # codec + validation, no large property tables
│   └── utf16                  # OS/interchange conversion
├── unicode                    # explicit properties/casing/normalization
├── io                         # standard streams, Reader/Writer, buffers, printing
│   └── raw                    # descriptor operations; deliberate imports
├── fs                         # files, metadata, directories, namespace operations
├── path                       # lexical paths; native default, explicit style
├── net                        # TCP/UDP/DNS over one existing reactor
│   └── raw                    # raw socket/handle escape hatch
├── time                       # Duration, Instant, SystemTime, Deadline
├── process                    # args/env/cwd/run/status
│   └── native                 # byte-preserving process inputs where needed
├── async                      # existing task model, composition, sleep/timeout
│   └── poll                   # advanced embedding/poll adapters
├── sync                       # Mutex/guard; bounded channel later
│   └── atomic                 # explicit SeqCst atomics
├── memory                     # Box, Shared
│   └── raw                    # raw byte/pointer operations beyond core substrate
├── numeric                    # checked arithmetic, parsing, formatting
├── limits                     # primitive bounds
├── math                       # floating functions/constants
├── binary                     # checked endian readers/writers
├── random                     # deterministic generator + OS entropy
├── native                     # explicit OS/ABI packages
│   ├── glibc
│   ├── pthreads
│   ├── kernel32
│   ├── ucrt
│   └── winsock
└── internal                   # compiler-facing runtime components
    ├── executor
    ├── network
    ├── threading
    └── platform
~~~

The internal tree is a design boundary, not a file-move instruction. DMM's internal-import restriction is module/subtree sensitive: verify that sibling public packages can reach a root internal package under actual loader rules. Retain private subtrees under async/net/sync instead if that is necessary. Do not loosen import safety merely to obtain a pretty tree.

Desired dependency direction:

~~~text
core -> no normal stdlib package
collections/memory -> core + explicit core memory
utf8/utf16/numeric/algorithms -> core (+ collections only for owning outputs)
text -> collections + utf8
io -> core + collections + numeric/text codecs
fs/process -> io + time/text codecs + internal platform
net/async/sync -> core + time + shared internal runtime mechanisms
unicode -> utf8/text + explicit generated tables
native -> ABI-safe declarations, no high-level façade dependency
~~~

Avoid root imports in implementation packages when a direct core import suffices. Avoid io↔fs and text↔collections cycles: String lives in text; collections' default text-key policy must use generic compile-time capability or an explicit text factory rather than importing text back into collections.

**Resolution of the text-key dependency:** collections supplies built-in primitive/string policies. `text.map<V>()` and `text.set()` select borrowed String policies and return the same nominal collections.Map/Set types. They are genuinely different constructors for a type collections cannot import, not aliases for discoverability. An alternative is a lower shared UTF-8 owner substrate; that adds complexity and is not recommended initially.

## 7. Proposed public APIs

The following are **contract sketches**, not compilable replacement files. Type declarations omit private storage and method bodies. Signatures use current DMM types, explicit borrows, implicit receivers, and slash imports. Mutating methods require exclusive receiver access; generic interface examples explicitly show their mut contract. Constructors and functions listed here are proposals, not APIs that exist today.

Error enums are package-specific. Native error details preserve domain/code; ordinary users can match portable kinds. Do not create a universal error enum covering allocation, filesystem, networking, parsing and cancellation.

### Core

~~~dmm
package core;

pub auto interface Copy {}
pub auto interface Send {}
pub auto interface Sync {}

pub enum Option<T> { None, Some(T) }
@[no_default] pub enum Result<T, E> { Ok(T), Err(E) }
pub enum AllocError { OutOfMemory, CapacityOverflow }

pub interface Iterator<T> {
    pub mut func next() -> Option<T>;
}

pub func mapOption<T,U>(value:Option<T>, f:once func(T)->U)->Option<U>;
pub func andThenOption<T,U>(value:Option<T>, f:once func(T)->Option<U>)->Option<U>;
pub func mapResult<T,U,E>(value:Result<T,E>, f:once func(T)->U)->Result<U,E>;
pub func mapError<T,E,F>(value:Result<T,E>, f:once func(E)->F)->Result<T,F>;
pub func andThenResult<T,U,E>(value:Result<T,E>, f:once func(T)->Result<U,E>)->Result<U,E>;
pub func unwrapOr<T>(value:Option<T>, fallback:T)->T;
pub func unwrapOrElse<T>(value:Option<T>, fallback:once func()->T)->T;
~~~

Keep current Propagation/FromResidual definitions and void Result handling; the abbreviated enum sketch is not permission to make void an ordinary enum payload.

### Collections

~~~dmm
pub func list<T>() -> List<T>;
pub func buffer<T:core.Copy>(count:usize, initial:T)
    -> core.Result<Buffer<T>,core.AllocError>;
pub func bufferWith<T>(count:usize, init:mut func(usize)->T)
    -> core.Result<Buffer<T>,core.AllocError>;

// List<T> methods:
length() -> usize
capacity() -> usize
reserve(capacity:usize) -> core.Result<void,core.AllocError>
append(value:T) -> core.Result<void,core.AllocError>
appendSlice(values:&T[]) -> core.Result<void,core.AllocError> where T:core.Copy
insert(index:usize, value:T) -> core.Result<void,core.AllocError>
get(index:usize) -> &T
getMut(index:usize) -> &mut T
pop() -> core.Option<T>
remove(index:usize) -> core.Option<T>
contains(value:&T) -> bit                // supported built-in equality types
contains(value:&T,equal:func(&T,&T)->bit) -> bit
clear() -> void
truncate(length:usize) -> void
view() -> T[]                   // origin retained, access uses checked descriptor
iter<'a>() -> ListIter<'a,T> where T:core.Copy
iterRef<'a>() -> ListRefIter<'a,T>
iterMut<'a>() -> ListMutIter<'a,T>
~~~

`list()` has valid empty state and allocates nothing. reserve never changes logical length. get/getMut and invalid insert indices trap as programmer errors; remove returns None out of range. Mutation through a slice must use a checked mutable descriptor and retain its backing origin. Do not add public owner indexing until the language actually supports that operation; `list[i]` is not assumed.

append/insert consume their input, including on allocation failure; it is destroyed exactly once on failure. A caller needing retention can reserve first, then transfer. List.contains uses borrowed equality and does not copy owners; custom equality is explicit for unsupported element types. Generic bufferWith initializes only constructed slots and cleans them on failure. It does not require T to be Copy or default-initializable.

Map/Set:

~~~dmm
pub func map<K,V>() -> core.Result<Map<K,V>,Error>;
pub func map<K,V>(hash:func(&K)->u64, equal:func(&K,&K)->bit)
    -> core.Result<Map<K,V>,Error>;
pub func set<T>() -> core.Result<Set<T>,Error>;
pub func set<T>(hash:func(&T)->u64, equal:func(&T,&T)->bit)
    -> core.Result<Set<T>,Error>;

// Map<K,V> methods:
length() -> usize
reserve(entries:usize) -> core.Result<void,Error>
insert(key:K, value:V) -> core.Result<void,Error>
get(key:&K) -> core.Option<&V>
getMut(key:&K) -> core.Option<&mut V>
contains(key:&K) -> bit
remove(key:&K) -> core.Option<V>
clear() -> void
iter<'a>() -> MapEntries<'a,K,V>
entriesMut<'a>() -> MapEntriesMut<'a,K,V>   // manual next, exclusive receiver

// Set<T> methods:
insert(value:T) -> core.Result<void,Error>
contains(value:&T) -> bit
remove(value:&T) -> core.Option<T>
length() -> usize
clear() -> void
iterRef<'a>() -> SetRefIter<'a,T>
~~~

Map replacement retains the existing key, destroys the incoming equivalent key, and replaces/destroys the old value. Failed insertion leaves the table unchanged and destroys incoming owners. Removal transfers the value; stored key cleanup occurs normally. Set removal transfers the actual stored value, useful for move-only elements.

Stored policies are noncapturing borrowed function values. Capturing policy closures are not promised by these signatures. Custom policies must obey equal-keys-have-equal-hash and stable-key rules. Built-in policies use specialization for supported scalars and primitive string; unsupported default key types fail at compilation. Owned String policies come from text as described above.

Default string hashing should be keyed against adversarial inputs. Constructor Error distinguishes allocation and entropy failure; custom deterministic hash callbacks remain an explicit choice. Reuse random.systemFill internally, not package-init hidden fatal entropy. A general public Hash interface is unnecessary.

### Text, UTF codecs and Unicode

~~~dmm
pub func string(value:string) -> core.Result<String,Error>;
pub func fromUtf8(bytes:&u8[]) -> core.Result<String,Error>;
pub func view<'a>(bytes:&'a u8[]) -> core.Result<Text<'a>,Error>;
pub func builder() -> StringBuilder;
pub func finish(builder:StringBuilder) -> String;

// String methods:
length() -> usize                    // bytes, never scalar/grapheme count
view<'a>() -> Text<'a>
startsWith(prefix:string) -> bit
endsWith(suffix:string) -> bit
contains(needle:string) -> bit
find(needle:string) -> core.Option<usize>
copyBytes(destination:&mut u8[]) -> core.Result<usize,Error>

// Text<'a> methods:
length() -> usize
byteAt(index:usize) -> u8
slice(start:usize,end:usize) -> core.Result<Text<'a>,Error>
startsWith(prefix:string) -> bit
endsWith(suffix:string) -> bit
contains(needle:string) -> bit
find(needle:string) -> core.Option<usize>
split(separator:u32) -> Split<'a>
codepoints() -> Codepoints<'a>

// StringBuilder methods:
append(value:string) -> core.Result<void,Error>
appendText<'a>(value:Text<'a>) -> core.Result<void,Error>
appendCodepoint(value:u32) -> core.Result<void,Error>
clear() -> void
length() -> usize
~~~

String/Text are validated UTF-8 and length-based; embedded NUL is preserved. Text keeps a checked source descriptor plus bounds, not an untracked raw pointer. Public Text operations do not expose a writable raw slice that could invalidate UTF-8. String accepts literal/primitive string input by copying and validating it. There is no implicit string-to-String allocation.

The literal overloads above scan/validate primitive input as required but allocate nothing. For String-to-String comparisons, matching Text overloads are useful: `contains(Text<'a>)`, `startsWith(Text<'a>)`, `endsWith(Text<'a>)`. They express a different storage contract and avoid conversion allocations. Do not add all possible conversion permutations.

StringBuilder owns its bytes; finish consumes it and should reuse its allocation. The transfer must use a reviewed consuming storage-transfer primitive implemented in DMM, not an assumed partial move of a checked field. If V tests fail, retain a documented copying finish for the first stage and do not claim zero-copy.

Primitive string cannot gain methods in the existing language. Keep focused free functions `text.length(string)`, `text.startsWith(string,string)`, `text.contains(string,string)`, etc. for literals and existing borrowed NUL strings. `text.bytes(string)` remains an explicitly raw/nonowning view; normal validated Text APIs should not depend on callers mutating it.

`text/utf8` retains strict decode/encode/validate, byte offsets and scalar validation. `text/utf16` retains explicit conversion for Windows/interchange. UTF conversions return owned List storage when allocating. `unicode` keeps properties, Category, full case mapping and four normalization forms. Scalar title casing must not be advertised as word-title formatting. Allocation-producing conversion names and Result contracts remain explicit.

### I/O

~~~dmm
pub interface Reader {
    pub mut func read(destination:&mut u8[]) -> core.Result<usize,Error>;
}
pub interface Writer {
    pub mut func write(source:&u8[]) -> core.Result<usize,Error>;
}

pub func stdin() -> Input;
pub func stdout() -> Output;
pub func stderr() -> Output;

pub func readAll<R:Reader>(source:&mut R)
    -> core.Result<collections.List<u8>,Error>;
pub func readAll<R:Reader>(source:&mut R, maximum:usize)
    -> core.Result<collections.List<u8>,Error>;
pub func readToEnd<R:Reader>(
    source:&mut R,target:&mut collections.List<u8>,maximum:usize
) -> core.Result<usize,Error>;
pub func readExact<R:Reader>(source:&mut R, destination:&mut u8[])
    -> core.Result<void,Error>;
pub func writeAll<W:Writer>(target:&mut W, source:&u8[])
    -> core.Result<void,Error>;
pub func copy<R:Reader,W:Writer>(source:&mut R,target:&mut W)
    -> core.Result<usize,Error>;
pub func bufferedReader<'a,R:Reader>(source:&'a mut R,capacity:usize)
    -> core.Result<BufferedReader<'a,R>,Error>;
pub func bufferedWriter<'a,W:Writer>(target:&'a mut W,capacity:usize)
    -> core.Result<BufferedWriter<'a,W>,Error>;

pub func print(value:string) -> core.Result<void,Error>;
pub func println(value:string) -> core.Result<void,Error>;
// Corresponding concrete numeric/bit/Text overloads, not Printable.
~~~

Input/Output are borrowed standard resources: destructors do not close standard descriptors. File resources own handles. read/write perform a single transfer; short success is normal. Reading a nonempty destination returns Ok(0) for EOF. Empty reads also return Ok(0), so callers must not infer EOF from a zero-sized request.

`io.Error` contains a portable kind, optional native domain/code, and `transferred:usize`. readExact/writeAll/copy report accumulated progress on failure. This replaces Transfer plus status plus separate lastError conventions. Preserve OutOfMemory, LimitExceeded, UnexpectedEof and WriteZero distinctions. A filesystem namespace error is not automatically this I/O error.

readAll owns its unpublished result and destroys partial data on error; progress is still reported. readToEnd appends into caller-owned storage, preserves appended bytes on failure, and reports newly appended count. This distinct in-place contract supports recovery without inventing a Result-plus-partial-data wrapper. Both share one implementation.

Bounded readAll fails instead of silently truncating. To distinguish exact-limit EOF, it may consume one extra probe byte; document that consequence for nonseekable streams. Callers requiring preservation must use a buffered reader with lookahead. Whole-file helpers can safely use this policy.

BufferedReader offers `read`, `readLine(maximum)->Result<Option<List<u8>>,Error>`, and the common read helpers. Line reading strips LF and optional preceding CR; an unterminated final line is returned once. No allocation occurs for EOF with no data.

BufferedWriter offers `write`, `writeAll`, `flush`. Failure retains unwritten data and retry state. Destruction frees memory but does not pretend to report a successful flush. Explicit flush is required before successful application completion. Documentation and tests must make dropped pending data visible; do not add an infallible hidden destructor flush.

Memory readers/writers and files justify Reader/Writer; networking is asynchronous and does not pretend to implement these synchronous interfaces. Async interface methods are not supported. A separate AsyncReader hierarchy is not proposed.

### Filesystem and paths

~~~dmm
pub func open(path:string) -> core.Result<File,Error>;       // existing file, read
pub func create(path:string) -> core.Result<File,Error>;     // create/truncate, write
pub func openAppend(path:string) -> core.Result<File,Error>;
pub func open(path:string, mode:OpenMode) -> core.Result<File,Error>;
pub func readFile(path:string,maximum:usize)
    -> core.Result<collections.List<u8>,Error>;
pub func readFile(path:string) -> core.Result<collections.List<u8>,Error>;
pub func writeFile(path:string,bytes:&u8[]) -> core.Result<void,Error>;
pub func metadata(path:string) -> core.Result<Metadata,Error>;
pub func exists(path:string) -> core.Result<bit,Error>;
pub func removeFile(path:string) -> core.Result<void,Error>;
pub func createDir(path:string) -> core.Result<void,Error>;
pub func createDirs(path:string) -> core.Result<void,Error>;
pub func removeDir(path:string) -> core.Result<void,Error>;
pub func rename(from:string,to:string) -> core.Result<void,Error>;
pub func openDir(path:string) -> core.Result<Directory,Error>;
pub func readDir(path:string) -> core.Result<collections.List<Entry>,Error>;

// File:
read(destination:&mut u8[]) -> core.Result<usize,io.Error>
write(source:&u8[]) -> core.Result<usize,io.Error>
readAll() -> core.Result<collections.List<u8>,io.Error>
readAll(maximum:usize) -> core.Result<collections.List<u8>,io.Error>
writeAll(source:&u8[]) -> core.Result<void,io.Error>
seek(offset:i64,origin:SeekOrigin) -> core.Result<u64,io.Error>
close() -> core.Result<void,io.Error>

// Directory:
next() -> core.Result<core.Option<Entry>,Error>
close() -> core.Result<void,Error>
~~~

File delegates its convenience methods to the generic I/O implementation. This intentional method/free-function pair serves resource-oriented and generic code without duplicating behavior.

Explicit close is idempotent and reports errors. Destructor close is best effort and never double-closes. Entries own their names; Metadata contains kind/size/SystemTime, not native stat structs.

Portable fs.Error keeps common kinds and native diagnostics, including NotFound, AlreadyExists, AccessDenied, InvalidPath, InvalidInput, OutOfMemory and System. fs.readFile/writeFile convert internal I/O failures while retaining progress and native details. Windows UTF conversion failure must not masquerade as OOM.

Paths use primitive string for common calls; byte-slice overloads preserve arbitrary non-NUL Linux paths and existing Windows UTF-8 validation. No implicit allocation of a Path wrapper. Byte overloads are a capability distinction, not a spelling alias. Path helpers include isAbsolute, basename, dirname, extension, join and clean; allocating outputs are explicit owned byte/text results. Lexical operations never claim filesystem/symlink resolution.

Streaming Directory is deliberately not forced into `for`: its factory would need mutable traversal, while plain loop factory access is shared. `readDir` produces an owning iterable List. Manual next supports large directories without allocation of the entire listing.

### Networking

~~~dmm
pub func resolve(host:string,port:u16)
    -> Future<core.Result<collections.List<Address>,Error>>;
pub func dial(host:string,port:u16)
    -> Future<core.Result<TcpStream,Error>>;
pub func dial(host:string,port:u16,deadline:time.Deadline)
    -> Future<core.Result<TcpStream,Error>>;
pub func listen(address:Address) -> core.Result<TcpListener,Error>;
pub func udpBind(address:Address) -> core.Result<UdpSocket,Error>;

// TcpListener:
accept() -> Future<core.Result<TcpStream,Error>>
accept(deadline:time.Deadline) -> Future<core.Result<TcpStream,Error>>
localAddress() -> core.Result<Address,Error>

// TcpStream:
read(destination:&mut u8[]) -> Future<core.Result<usize,Error>>
write(source:&u8[]) -> Future<core.Result<usize,Error>>
writeAll(source:&u8[]) -> Future<core.Result<void,Error>>
peerAddress() -> core.Result<Address,Error>
localAddress() -> core.Result<Address,Error>
shutdownWrite() -> core.Result<void,Error>

// UdpSocket:
receive(destination:&mut u8[]) -> Future<core.Result<Datagram,Error>>
send(source:&u8[],peer:Address) -> Future<core.Result<usize,Error>>
~~~

Include deadline overloads for transfers where existing engine support exists. Avoid a builder for ordinary connections. Address has concrete IPv4/IPv6 constructors and formatting/parsing; native sockaddr remains private.

TcpStream/TcpListener/UdpSocket encode genuinely different protocol states and operations. They use the existing backend socket owner and reactor. They are not three competing networking implementations.

Retain one read and one write slot per connected stream, safe full-duplex behavior, in-flight resource retention, checked buffer loans, cancellation acknowledgment, DNS job ownership, and shutdown guarantees. A shared receiver permits one read and one write concurrently; that existing interior state is implementation-private. Do not change all methods to exclusive receiver access and accidentally destroy full duplex.

TCP read EOF uses the same nonempty-buffer Ok(0) rule. UDP Datagram keeps source address, transferred bytes and truncation information; those cannot be represented by a byte count alone. Network Error has portable kind, native diagnostics and partial progress where relevant. No silent discard of writeAll progress.

Host literal storage is copied or otherwise retained before asynchronous DNS work can outlive it; mutable caller storage remains borrowed where accepted. Futures retain socket and buffer origins until completion/cancellation acknowledgment. Destructors close idle owners; explicit close/shutdown behavior must preserve current in-flight checks.

### Time

~~~dmm
pub func nanoseconds(value:u64) -> Duration;
pub func milliseconds(value:u64) -> core.Result<Duration,Error>;
pub func seconds(value:u64) -> core.Result<Duration,Error>;
pub func now() -> core.Result<Instant,Error>;
pub func systemNow() -> core.Result<SystemTime,Error>;
pub func sleep(duration:Duration) -> core.Result<void,Error>;
pub func after(duration:Duration) -> core.Result<Deadline,Error>;

pub enum Deadline { Never, At(Instant) }

// Duration: nanos(), checkedAdd()
// Instant: elapsed(), checkedAdd(Duration), durationSince(Instant)
// SystemTime: seconds(), nanos(); no implicit conversion to Instant.
~~~

Keep constructors checked where multiplication/addition can overflow. Wall time and monotonic time remain different types. Blocking time.sleep and asynchronous tasks.sleep are semantically distinct operations in different packages.

### Process and environment

~~~dmm
pub func args() -> core.Result<collections.List<text.String>,Error>;
pub func env(name:string) -> core.Result<core.Option<text.String>,Error>;
pub func setEnv(name:string,value:string) -> core.Result<void,Error>;
pub func removeEnv(name:string) -> core.Result<void,Error>;
pub func cwd() -> core.Result<text.String,Error>;
pub func setCwd(path:string) -> core.Result<void,Error>;
pub func run(program:string,args:&string[]) -> core.Result<ExitStatus,Error>;

pub enum ExitStatus { Code(i32), Signal(i32) }
~~~

args excludes the executable name; a separate executablePath operation is useful only when platform semantics are correctly implemented. Each args call returns independently owned storage. No borrowed getenv pointer escapes.

Text args/env/cwd report invalid encoding explicitly. `process/native` provides byte-preserving args/env/cwd on platforms whose native data permits non-UTF-8. Returning `List<List<u8>>` is verbose but accurately expresses ownership without introducing an OsString hierarchy merely to rename bytes. Windows conversion is UTF-16-aware.

Portable argv requires startup capture. Existing parameterless main does not expose it; this is R/B work, not a library helper that already works. Env methods synchronize library-controlled mutation/read access; this cannot make unsynchronized foreign getenv/setenv calls safe.

run directly invokes a program with argv; it does not interpret a shell command. It waits/reaps before returning. New posix_spawn/wait and Windows CreateProcess/wait bindings are needed. Avoid fork from a running multithreaded executor. Independent Child and pipe APIs wait for a separate lifecycle review.

Do not provide a normal exit function that casually skips destructors. Return an integer from main. Immediate native exit stays explicit.

### Async, synchronization and memory

Use an alias for the async package:

~~~dmm
import tasks "stdlib/async";
~~~

Keep compiler-facing `block_on`, `spawn`, `cancel`, Executor.create, executor shutdown, JoinHandle result/await semantics and their exact current obligations. They are not ordinary functions the library can rename independently.

~~~dmm
// Normal async package:
pub func join<T,U>(left:Future<T>,right:Future<U>) -> Future<Joined<T,U>>;
pub func select<T,U>(left:Future<T>,right:Future<U>) -> Future<Selected<T,U>>;
pub func race<T>(left:Future<T>,right:Future<T>) -> Future<T>;
pub func asUnit(value:Future<void>) -> Future<Unit>;
pub func sleep(duration:time.Duration) -> Future<void>;
pub func timeout<T>(value:Future<T>,duration:time.Duration)
    -> Future<Timed<T>>;

// Selected: Left(T,Future<U>) or Right(Future<T>,U).
// Timed: Completed(T) or Elapsed.
// Void-specific overloads/adapters remain where generic fields cannot store void.
~~~

join waits for both; select transfers the pending loser to the caller; race waits for loser cancellation acknowledgment before returning; timeout likewise waits for cancellation acknowledgment. These are distinct justified APIs.

Advanced `async/poll` retains Poll, Context, Waker, Poller, fromPoller, poll and pollVoid, including pending-owner transitions and cancelPoll acknowledgment. Noop contexts belong there, not in normal examples.

~~~dmm
pub func mutex<T:core.Send>(value:T) -> core.Result<Mutex<T>,sync.Error>;
pub func lock<'a,T:core.Send>(value:&'a Mutex<T>)
    -> core.Result<MutexGuard<'a,T>,sync.Error>;

pub func box<T>(value:T) -> core.Result<Box<T>,core.AllocError>;
pub func intoInner<T>(value:Box<T>) -> T;
pub func shared<T>(value:T) -> core.Result<Shared<T>,core.AllocError>;
~~~

MutexGuard borrows the owner, is non-Send, and unlocks on destruction. Box and Shared methods return checked references, never writable shared storage. Shared clone increments ownership; Send/Sync requires the appropriate payload properties. Existing atomics remain SeqCst under an explicit import.

A later bounded channel should own Send messages, wake existing executor tasks through Poller, and return an unsent payload on closed/canceled sends. Cancellation may not silently destroy a value unless the documented consuming operation says so. These contracts must be resolved before selecting final send/receive signatures.

### Algorithms, numeric, binary, random

Keep this surface focused:

* algorithms: borrowed find/count/all/any, slice equality with borrowed callback, reverse/sort, lowerBound/binarySearch, Copy copy/fill/transform, forEachMut. Mutation requires checked mutable slice access. Sort does not promise stability unless separately implemented.
* numeric: strict parseInt/parseUint plus float parsing, checked integer arithmetic, format into caller-provided checked buffers. Formatting reports written count and leaves the buffer unchanged on insufficient capacity where the existing contract supports this.
* limits: one set of primitive constants; no aliases in numeric.
* math: current common floating functions/constants/classification; min/max/clamp use concrete supported overloads with documented NaN/signed-zero behavior.
* binary: `reader<'a>(&'a u8[])->Reader<'a>`, `writer<'a>(&'a mut u8[])->Writer<'a>`; read/write widths and Endian; position/remaining; all-or-nothing bounds errors.
* random: explicitly seeded deterministic Generator, bounded unbiased generation, systemFill into checked mutable bytes. Partial entropy failure is an error, never a fallback to the deterministic generator.

## 8. Iterator design

### Fundamental protocol

The compiler's actual loop protocol is authoritative:

| Loop | Factory | next result | Source access |
|---|---|---|---|
| `for (var x = source)` | iter() | canonical Option<Item> | Shared |
| `for (var &x = source)` | iterRef() | Option<&T> | Shared |
| `for (var &mut x = source)` | iterMut() | Option<&mut T> | Exclusive |

Factories and next are parameterless, concrete instance methods. Method type-generic factories are not supported; lifetime parameters are available. The iterator result is a concrete struct, not an associated-type interface value. next mutates iterator state.

A named source is borrowed, not consumed. A temporary owner is retained by the loop. Iterator cleanup happens on normal exit, break, continue and return as appropriate. The mutable item's loan is iteration-local: it cannot be stored outside the iteration, returned, or captured by an escaping future.

`core.Iterator<T>` expresses the next contract for generic helpers. It does not replace compiler factory validation or magically provide associated Item types.

### Container behavior

List/Buffer:

* iter for Copy T returns copied values.
* iterRef works for all T and yields checked shared references.
* iterMut works for all T and yields exclusive iteration-local references.
* Mutable iteration freezes structural operations; append/remove/reserve cannot invalidate a live iterator.
* Move-only elements are read with `for(var &x=items)`, mutated with the mutable loop, and consumed explicitly through pop/remove or a separately reviewed consuming cursor.

Map:

* iterRef yields an entry view with checked key and value borrows.
* iterMut yields a shared key and mutable value; mutating a key would invalidate hashing.
* Returned entry references have the map receiver origin.
* Structural mutation during iteration is forbidden.
* Order is unspecified; no hidden ordered-map promise.

The compiler's `for(var &x)` form requires Option<&T>, not an arbitrary aggregate entry view. Therefore Map entry-view iterators should use plain `iter()` with Item = EntryRef<'a,K,V>, or explicit next calls; do not claim iterRef returning EntryRef satisfies the reference loop contract. Final Map signatures must select this concrete shape:

~~~dmm
// Map methods:
iter<'a>() -> MapEntries<'a,K,V>            // next -> Option<EntryRef<'a,K,V>>
entriesMut<'a>() -> MapEntriesMut<'a,K,V>   // manual next, exclusive map borrow
~~~

A mutable EntryRef containing references must obey the compiler's iteration-local escape checks. Because plain loop factory access is shared, entriesMut is manual initially. Do not extend the language protocol to make Map mutable-entry syntax look like Rust. A key-only Set.iterRef naturally fits the reference loop.

Text:

* Split owns traversal state, borrows Text and yields Text views; its iter factory creates fresh cursor state.
* Codepoints over validated Text yields u32 directly and fits the plain loop.
* Raw unvalidated UTF-8 iteration yields Option<Result<u32,Utf8Error>> and terminates after one decoding error. No separate fallible-loop language feature is required.

Directory remains Result<Option<Entry>> manual traversal. Eager readDir returns List<Entry> and therefore works with reference iteration.

### Move-only and producer iteration

Plain iteration does not consume a container. A producer iterator may own internal state and produce new move-only values through next. This is different from moving elements out of a named List.

Do not promise `for(var value=list)` drains a move-only list. The current syntax does not express consumption. A draining cursor can be added later as an explicit consuming free function, but its factory/shared-access behavior must be proven. No new consuming-loop syntax is proposed.

### Compact convenience layer

First implementation:

~~~dmm
pub func fold<T,A,I:core.Iterator<T>>(
    iterator:&mut I, initial:A, step:mut func(A,T)->A
) -> A;

pub func map<T,U,I:core.Iterator<T>>(
    iterator:&mut I, mapper:mut func(T)->U
) -> core.Result<collections.List<U>,core.AllocError>;

pub func filter<T,I:core.Iterator<T>>(
    iterator:&mut I, predicate:mut func(&T)->bit
) -> core.Result<collections.List<T>,core.AllocError>;

pub func enumerate<T,I:core.Iterator<T>>(
    iterator:&mut I
) -> core.Result<collections.List<Indexed<T>>,core.AllocError>;
~~~

map/filter/enumerate above are explicitly **eager allocating operations**. fold is streaming and does not allocate by itself. Names reside in `iter`, and documentation must lead with allocation/consumption. Allocation failure leaves the source partially advanced and destroys any unpublished produced output. No Copy constraint is needed for owned produced items.

When Item is a checked shared reference, collected output retains the source origin. Mutable lending items must not be collected or retained by these operations. This cannot be expressed by inventing a negative generic bound: specialization and borrow checking must reject such escaping uses. If that rejection cannot be proven with current semantics, ship the convenience layer only for nonlending producers and verified shared-reference cursors; mutable iteration remains the compiler-checked loop path. The existing Iterator<T> interface has no higher-ranked lifetime machinery to express a universal lending iterator; mutable conveniences are limited to direct iteration and forEachMut.

Lazy closure-storing map/filter are later V work. Dynamic interface erasure just to store callbacks would add allocation and Send/lifetime ambiguity; avoid it. No zip/chain/cycle/peekable/multi-step adapter catalog is proposed.

## 9. Low-level/platform boundary

| Layer | Contents | Ordinary user visibility |
|---|---|---|
| Core | Canonical properties, absence/failure/propagation, fundamental iteration | Common |
| Explicit core memory | Generated allocator and element lifetime substrate | Container implementers |
| Portable stdlib | Text/collections/I/O/fs/net/time/process/async/sync | Normal application code |
| Internal DMM implementation | Executor entry ABI, wake ownership, reactor, DNS workers, timer scheduling, platform shims | No normal imports |
| Explicit raw packages | Descriptor operations, pointer arithmetic/byte copying, raw socket handles | Deliberate opt-in |
| Native packages | glibc/pthreads/kernel32/UCRT/Winsock layouts, constants, externs | OS/ABI programming |

Normal errors may expose a native domain/code for diagnostics; that does not justify exposing native handle ownership through every portable object.

Raw pointer access requires explicit dereference. No checked-reference auto-deref rule should be extended to raw pointers. Do not put unsafe-looking operations behind a convenient façade without documenting the trust contract; DMM need not invent an unsafe block keyword to express this boundary.

Prefer DMM implementations over new C bridges when FFI can describe the ABI. Keep C/native reference tests and ABI headers where they validate behavior; they are not automatically obsolete public compatibility layers.

Compiler migration must account for:

* automatically loaded `stdlib/system` provider;
* hardcoded runtime component retention for executor/network paths;
* private `__dmm_` ABI names and native record layouts;
* concrete Future capture metadata and source Send/Sync checking;
* generated allocation versus native malloc domains.

Generated allocator pointers must not be freed through libc. Runtime-native allocation must not be released by core memory. Refactoring package names does not change that rule.

Platform suffix files remain target-selected before parsing. New functionality needs Linux and Windows implementations with explicitly documented unsupported targets, not a fictional portable fallback.

## 10. Ownership/API review

| API | Input/receiver | Output/allocation | Failure and lifetime rules |
|---|---|---|---|
| list() | None | Valid empty owner, no allocation | No failed-object state |
| List.append/insert | Exclusive receiver, consumes T | May allocate/grow | Error destroys incoming T once; old collection unchanged |
| List.get/getMut | Shared/exclusive receiver | Checked reference, no allocation | Borrow retains owner; mutation/reallocation conflicts |
| appendSlice | Checked shared slice, Copy T | May allocate | Disjointness or safe alias handling explicit; no universal snapshot allocation |
| bufferWith | Consumes/invokes initializer | Owning fixed initialized elements | Partial initialization cleaned correctly; no Copy/default requirement |
| Map.insert | Exclusive receiver, consumes K/V | May allocate | Atomic table growth; stable key policy; no move-only copies |
| Map.get/remove | Borrowed key | Borrowed V / owned V | Origin from map, not from lookup key; removed key destroyed |
| String construction | Borrowed primitive string/bytes | Owns copied validated UTF-8 | Alloc/encoding error explicit; no hidden implicit conversion |
| Text view/slice | Checked shared descriptor/receiver | Nonowning validated view | Retains backing origin; byte bounds at UTF-8 boundaries |
| StringBuilder finish | Consumes owner | Transfers allocation if verified | No live emptied builder; no unsupported tracked partial move |
| io.readAll | Exclusive Reader borrow | Owns List<u8>, allocation explicit | Limit policy and partial read advancement documented |
| readExact/writeAll | Checked destination/source | No necessary allocation | Error includes total progress |
| BufferedReader/Writer | Checked exclusive resource borrow | Owns buffer | Cannot outlive resource; flush failures remain observable |
| File | Owns OS handle | No exposed native layout | Destructor cleanup; explicit close reports failure |
| Directory.next | Exclusive Directory | Owned Entry name | None is end; errors are real failures |
| net.read/write | Retains socket and checked buffer origins | Future frame may allocate | No release until completion/cancellation acknowledgment |
| spawn | Consumes concrete Future | Owned JoinHandle | Send captures checked; caller borrows cannot escape |
| select | Consumes two Futures | Winner + still-owned loser Future | Caller must complete/cancel loser |
| race/timeout | Consumes Future owners | Value/status | Waits cancellation acknowledgment before releasing losers |
| MutexGuard | Borrows Mutex | Exclusive protected borrow | Non-Send; unlock destructor |
| Shared.clone | Shared owner access | Refcounted owner; no payload copy | Payload-dependent Send/Sync; no mutable shared getter |
| process.args/env | Borrowed names | Independent owned text | Raw native byte option explicit; no getenv borrow escapes |
| process.run | Borrowed program/argv | ExitStatus | argv marshaled; process waited/reaped before return |

### Allocation visibility

Future frame construction currently may fail fatally; ordinary library Results do not change that runtime contract. The replacement must document this separately rather than implying every asynchronous allocation is recoverable.

String concatenation on primitive string is not an RAII text builder. Keep that language behavior visible in documentation; use StringBuilder for normal owned construction.

The initial library should not offer zero-copy conversions merely because they would be attractive. Prove storage ownership transfer and destructor state first. Copying conversions must say they copy.

### Borrow verification gates

Before implementation acceptance, demonstrate:

1. List/Buffer view descriptor origins survive through Reader/Text/Binary adapters.
2. String.view retains the whole receiver without leaking a writable raw slice.
3. Map lookup result origin is only the map, not an unrelated temporary key.
4. Map entry views preserve key/value origins and forbid structural mutation.
5. Mutable loop items cannot escape through collected output, closures, or spawned tasks.
6. Stored borrowed iterator outputs preserve source lifetime.
7. Async socket/read buffer retention survives timeout, select and cancellation.
8. Shared/Mutex auto-property derivation remains correct after package moves.

No higher-ranked trait, implicit allocation, implicit borrow, or associated type is assumed to pass these gates.

## 11. Migration plan

**Do not execute this plan until the audit is reviewed and explicitly approved.**

Stages are ordered to avoid combining canonical-identity changes with ownership/API rewrites.

### Stage 1 — Freeze contracts and establish foundations

* Approve package/API decisions, including eager iterator conveniences and checked Text representation.
* Record current behavior and negative ownership/cancellation contracts.
* Establish canonical core Option/Result/propagation and AllocError without changing nominal identity accidentally.
* Default Option to None; reject Result default construction.
* Separate core memory substrate and document allocator domains.
* Keep current compiler-resolved import identities until B tests exist.

Acceptance: focused propagation, consuming enum, Copy/Send/Sync, allocator lifetime and default-state contracts pass. No unrelated implementation added.

### Stage 2 — Migrate owning sequences and their users

* Move List/Buffer into collections; replace Bytes with List<u8>.
* Introduce valid empty constructors, Result operations and conventional append.
* Migrate tests/examples/runtime consumers before deleting old names.
* Add real iterator factories for List/Buffer and prove mutable-item escape rejection.
* Introduce checked view contracts required by downstream adapters.

Acceptance: allocation fault tests, move-only element drops, self/overlap contracts, borrow origins and loop cleanup pass.

### Stage 3 — Text and interoperable I/O

* Establish UTF codec layer independently of large Unicode tables.
* Implement String/Text/StringBuilder and prove ownership/lifetime invariants.
* Add Reader/Writer and standard stream types.
* Replace manual-release stdio storage/adapters with checked RAII buffers.
* Consolidate file/stream helpers and preserve partial progress.
* Migrate printing and examples; remove sticky/last-error protocols.

Acceptance: embedded NUL, invalid UTF-8, boundary slicing, short I/O, EOF, buffering retry, bounded read probes, close/drop and allocation failures pass.

### Stage 4 — Consolidate existing higher-level APIs

* Simplify Map/Set policies and owned keys; add default hashing policy and constructor error contract.
* Merge copy/borrowed algorithm twins where the operation permits it.
* Consolidate binary readers/writers, limits and numeric helpers.
* Preserve Unicode conformance and strict codecs.
* Remove Cell/Printable/Equal, Deque/Arena and obsolete public aliases.

Acceptance: hash collisions/replacement/remove/fault tests, move-only keys, callback no-allocation, algorithm overlap, binary all-or-nothing and Unicode conformance pass.

### Stage 5 — Portable fs/net/async surfaces

* Normalize filesystem error kinds and Directory EOF behavior.
* Move networking façade to net over the existing backend.
* Preserve full duplex, completion retention, DNS cancellation and shutdown ordering.
* Move poll API to advanced package; rename library composition helpers only.
* Share deadline type and migrate timer implementation without creating a new executor.
* Retain exact compiler-owned task spellings and obligations.

Acceptance: existing Linux networking cancellation/ownership contracts plus Windows ABI/CI coverage; timer acknowledgment, loser ownership, graceful shutdown and future Send contracts pass.

### Stage 6 — Add missing application fundamentals

* Capture argv at startup through a narrowly scoped runtime contract.
* Add process args/env/cwd and byte-preserving native variants.
* Implement direct synchronous process.run with reviewed platform FFI.
* Add createDirs, practical line/text/numeric helpers and memory I/O adapters.
* Add bounded channel only after its payload-retention and cancellation contract is approved.

Acceptance: argument ownership/encoding, env absence/races under library control, cwd, process argv quoting and reaping, portable exit status, and filesystem race/error tests pass.

### Stage 7 — Migrate platform/runtime implementation boundaries

* Retarget source provider and runtime component paths deliberately.
* Keep __dmm ABI layouts and allocation domains stable unless separately revised.
* Move private scheduler/reactor/threading APIs behind working internal-import boundaries.
* Remove core forwarding of I/O/process/shared/atomic/application facilities.
* Consider standalone core import identity only as a separate compiler-binding change.

Acceptance: package expansion, provider loading, target filtering, standalone runtime, generated/native ABI parity and module visibility contracts pass.

### Stage 8 — Remove old surfaces and finish documentation

* Delete deprecated APIs only after every in-repository consumer is migrated.
* Publish package responsibility, allocation and failure tables.
* Compile the final target examples on supported targets.
* Run the appropriate Linux contract suite and cross-target CI, following AGENTS.md.
* Perform one final public API/dependency audit; reject unexplained aliases or accidental root exports.

Approval of this document alone should define the scope of a subsequent implementation task; it does not start it automatically. Independent spawning, lazy adapters and other later candidates need their own acceptance gates.

## 12. Removal list

| Remove/replace | Replacement |
|---|---|
| Root owning Bytes and stdio.Bytes | collections.List<u8> |
| Failed collection/string objects with ok/error state | Valid empty objects or Result constructors |
| AllocationError.None and InvalidIndex | Result.Ok; bounds contract, not allocation error |
| Root Input/System/open_read/open_write/close_file/read_file/write_file | io Reader/Writer, fs File; explicit io/raw for descriptors |
| Root print/println exports and public fixed-print helpers | io.print/println; private common formatter |
| stdio Copy/manual-close Stream | RAII File and borrowed Input/Output |
| stdio Transfer/ByteResult/status/lastError protocol | Result plus io.Error with progress |
| stdio raw-pointer/manual-release buffered wrappers | Checked BufferedReader/Writer |
| stdio duplicate readFile/writeFile | fs.readFile/writeFile |
| Cell | Ordinary fields/variables |
| Printable/printValue | Explicit text/scalar output |
| Equal by-value interface | Borrowed comparator at the operation requiring it |
| Public Deque | External package if needed; private executor queue stays |
| Public byte Arena | External allocation package if needed |
| HashMap/HashSet names | Map/Set |
| CopyCallbacks/BorrowedCallbacks/KeyCallbacks erasure family | Borrowed static hash/equality functions and supported default policies |
| getCopy/viewCopy and copy/borrowed spelling twins without distinct contracts | Borrow-first API and explicit Copy dereference |
| Raw String.view()->primitive string model | Checked length-based Text view |
| Stateless binary cursor that takes buffer on each operation | Checked buffer-owning borrowed cursor |
| Duplicate limits in numeric, bit min/max | Canonical limits; true/false directly |
| Unicode categoryCode numeric encoding | Category enum |
| Directory EndOfDirectory error | Result<Option<Entry>> |
| Shared and atomics exported from core | memory.Shared and sync/atomic |
| Poll and application/runtime facilities exported from core | async/poll and internal runtime packages |
| Public normal Timer state with started-drop trap | async sleep/timeout; internal scheduling |
| join2/select2/race2 names | join/select/race, preserving owner contracts |
| core/net public façade | net; existing engine remains |
| Native handles/layouts exposed by portable façade | Explicit raw/native imports |
| Public private-threading/runtime ABI entry points | Internal compiler-facing components |

Do not delete runtime ABI definitions, source provider mechanisms, native ABI tests or generated Unicode data merely because they are large or have private-looking names. Their mechanisms remain; public placement and forwarding change.

## 13. Final target examples

These illustrate the **proposed replacement**, not code runnable against the current stdlib. They use parameterless main returning int and explicit borrowed arguments. Error conversion examples deliberately retain package-specific errors; there is no universal stdlib error.

### Simple CLI arguments

~~~dmm
package main;
import (
    core "stdlib/core"
    "stdlib/process"
    "stdlib/io"
);

func main() -> int {
    match (process.args()) {
        Ok(args) => {
            for (var &arg = args) {
                match (io.println(arg.view())) {
                    Ok => {}
                    Err(error) => return 1;
                }
            }
            return 0;
        }
        Err(error) => return 1;
    }
}
~~~

### Reading and processing a file

~~~dmm
package main;
import (
    core "stdlib/core"
    "stdlib/fs"
    "stdlib/text"
    "stdlib/io"
);

enum AppError {
    File(fs.Error), Text(text.Error), Output(io.Error),
    ;
    static func fromResidual(error:fs.Error)->Self { return AppError.File(error); }
    static func fromResidual(error:text.Error)->Self { return AppError.Text(error); }
    static func fromResidual(error:io.Error)->Self { return AppError.Output(error); }
}

func run() -> core.Result<void,AppError> {
    var bytes = fs.readFile("notes.txt", 1048576)?;
    var storage = bytes.view();
    var content = text.view(&storage)?;
    var lines = content.split(10);
    for (var line = lines) {
        if (line.contains("DMM")) { io.println(line)?; }
    }
    return core.Result<void,AppError>.Ok;
}

func main() -> int {
    match (run()) { Ok => return 0; Err(error) => return 1; }
}
~~~

### Writing a file

~~~dmm
func save() -> core.Result<void,fs.Error> {
    var data = text.bytes("hello\n");
    fs.writeFile("hello.txt", &data)?;
    return core.Result<void,fs.Error>.Ok;
}
~~~

text.bytes here is a deliberate raw view of a static literal, immediately borrowed for writing. It does not allocate or transfer literal storage.

### List and iterator processing

~~~dmm
func positive(value:&int) -> bit { return *value > 0; }

func processNumbers() -> core.Result<int,core.AllocError> {
    var values = collections.list<int>();
    values.append(-2)?;
    values.append(3)?;
    values.append(4)?;

    for (var &mut value = values) { *value *= 2; }

    var cursor = values.iter();
    var selected = iter.filter(&mut cursor, positive)?;
    var total: int = 0;
    for (var value = selected) { total += value; }
    return core.Result<int,core.AllocError>.Ok(total);
}
~~~

The generic arguments are inferred from the concrete iterator and noncapturing predicate. No partial explicit generic argument syntax is assumed.

### Map and Set

~~~dmm
func count() -> core.Result<int,collections.Error> {
    var counts = collections.map<int,int>()?;
    var seen = collections.set<int>()?;
    counts.insert(7, 3)?;
    seen.insert(7)?;

    var key: int = 7;
    if (!seen.contains(&key)) { return core.Result<int,collections.Error>.Ok(0); }
    match (counts.get(&key)) {
        Some(value) => return core.Result<int,collections.Error>.Ok(*value);
        None => return core.Result<int,collections.Error>.Ok(0);
    }
}
~~~

### TCP client

~~~dmm
async func fetch() -> core.Result<void,net.Error> {
    var socket = net.dial("127.0.0.1", 9000).await()?;
    var request = text.bytes("ping\n");
    socket.writeAll(&request).await()?;

    var response: u8[256];
    var destination: u8[] = response[:];
    var count = socket.read(&mut destination).await()?;
    // response[:count] contains bytes; decoding is explicit if needed.
    return core.Result<void,net.Error>.Ok;
}
~~~

Future/socket/buffer loans end after awaited completion. These examples use the existing `.await()` syntax followed by `?`; no prefix-await language syntax is proposed.

### Async task with explicit completion

~~~dmm
package main;
import (
    core "stdlib/core"
    tasks "stdlib/async"
);

async func answer() -> int { return 42; }

async func joined(handle:JoinHandle<int>) -> int {
    var result = handle.await();
    match (result) {
        Ok(value) => return value;
        Err(error) => return -1;
    }
}

func main() -> int {
    var executor = Executor.create(2);
    var handle = executor.spawn(answer());
    var value = executor.block_on(joined(handle));
    block_on(executor.shutdown(ShutdownMode.Drain));
    if (value == 42) { return 0; }
    return 1;
}
~~~

The executor/JoinHandle calls above preserve the existing working syntax and TaskError result handling. Async examples require the manifest's async feature.

### Deliberate native/system access

~~~dmm
package main;
import (
    raw "stdlib/memory/raw"
    "stdlib/native/glibc"
);

func main() -> int {
    // Native malloc domain, explicitly paired with native free.
    var storage = glibc.malloc(64);
    if (storage == raw.null<void>()) { return 1; }
    glibc.release(storage);
    return 0;
}
~~~

malloc/release already exist in the curated bindings. This allocation must not be released through the generated allocator. A normal portable owner would use memory.Box or collections.List instead.

---

**Review decisions requested before implementation:** approve the core boundary, consolidation on List<u8>, validated length-based text/view contract, Reader/Writer error-progress model, Map policy constructors, compact iterator strategy, and staged runtime/package migration. No source implementation, file deletion, compatibility adaptation or test execution is part of this audit.
