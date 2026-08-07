# Compiler architecture

This document covers the compiler and private runtime contracts. Public library APIs live in the
[standard-library packages](stdlib/README.md).

- [Pipeline](#pipeline) and [component ownership](#component-ownership)
- [AST and semantics](#ast-and-semantic-model), [typed IR](#typed-ir) and [optimization](#ir-optimization)
- [Native backend and linking](#native-x86-64-backend)
- [Executor and platform runtime](#executor-runtime-and-native-abi)
- [Debugging dumps](#compiler-debugging-dumps) and [diagnostics](#compiler-diagnostics)

## Pipeline

The compiler has a single production pipeline:

```text
source file
  -> token stream
  -> arena-owned structured AST
  -> module manifest and package graph loading
  -> package-wide symbol collection with file-local import bindings
  -> semantic model and typed AST annotations
  -> verified target-neutral IR
  -> IR peephole/dataflow optimization and verification
  -> target-aware structured x86-64 instructions
  -> assembly printing/cleanup OR direct encoding
  -> ELF/COFF object serialization OR internal ELF/PE executable linking OR external platform linking
```

`dmm manifest sync` reuses package graph loading across all source packages of the selected module. It reconciles direct
and indirect requirements, checks exact versions, and atomically replaces `dmm.manifest` after a successful analysis. It
does not require executable entry points, lower IR, or invoke the backend.

Frontend or semantic errors stop compilation before IR creation. IR verification failures are compiler errors. The
backend never reparses source tokens and there is no alternate compatibility emitter.

Generic declarations remain arena-owned templates. `src/ast/generics.c` clones concrete specializations, substitutes
nested types and derives canonical identities. Semantic analysis infers call arguments, enforces invariant substitutions
and structural interface bounds, and checks each concrete body through a growing work list. The IR skips templates.
Sum construction, tag tests and guarded payload extraction are explicit operations; exhaustive matches become ordinary
branches and labels. The verifier rejects extraction without the matching guarded predecessor. Native aggregate layout
reserves a tag slot and the largest payload, preserving by-value copying through the existing internal ABI.
Interface values use a fixed two-word descriptor: a deterministic concrete type tag and an owned payload pointer.
Variables, parameters, returns, fields, variant payloads, arrays and slices share this representation. Dynamic calls
select the concrete method by tag; destruction dispatches to concrete drop glue and releases the payload.

## Component ownership

Auto-interface identities and explicit rules are represented in the AST and substituted with generic aggregates.
`semantic_satisfies` in `semantic_interfaces.c` is the common property/constraint query. Canonical stdlib/core Send and
Sync declarations bind to language roles; their spellings do not grant roles to other interfaces. Auto properties do
not enter structural implementation selection or interface-table emission. `semantic_requires_explicit_init` is a
separate monotone query over concrete representations, including first-variant enum defaults.

Ownership dataflow tracks live, never initialized, moved, and explicitly destroyed storage. The latter three states
have no live value, with distinct diagnostics. Transparent wrappers around initialize/destroy intrinsics retain their
effects at callers. Typed IR preserves lifetime starts as INIT and explicit ends as DESTROY; replacement retains drop
and reinitialization effects, including runtime flags at mixed control-flow joins. Raw heap operations retain the
caller's lifetime contract. Shared uses these general mechanisms entirely as stdlib source; there is no Shared type
name special case in the compiler. The dependency-free core/raw implementation keeps core -> stdlib -> raw acyclic.

This section assigns implementation responsibilities. Source-language ownership and borrow semantics are defined only
in [LANGUAGE_SPEC.md](LANGUAGE_SPEC.md).

- `src/frontend/lexer.c` owns lexical analysis and token diagnostics.
- `src/frontend/syntax_parser.c` constructs all declaration, type, statement, and expression nodes in the AST arena.
- `src/frontend/frontend.c` and `package_loader.inc` own module manifests, package discovery and the package graph.
  Imports resolve canonical package paths through the module root, declared vendor dependencies or bundled `stdlib`.
- `src/common/string_interner.c` owns the module-wide canonical spelling table. Root files and imports share it, so
  equal source strings have pointer identity.
- `src/ast` owns program lifetime, the shared string interner, spans, and AST storage.
- `src/ast/ast_optimize.c` simplifies typed function bodies after semantic analysis and before IR lowering at `-O1`.
- `src/ast/ast_dump.c` serializes the resolved tree as versioned `dmm-ast-v6`.
- `src/sema` collects package/member/local symbols, resolves file-local imports and named types, validates scopes,
  calls, conversions, lvalues, returns, bounds, and control-flow placement, and annotates AST nodes with stable IDs and
  types. `semantic_expressions.c` checks expressions; `semantic_analysis.c` checks statements and control flow;
  `semantic_generics.c` handles specialization and type normalization, `semantic_constants.c` evaluates
  constant expressions, `semantic_diagnostics.c` formats errors, `semantic_layout.c` computes aggregate storage,
  and `semantic_interfaces.c` checks structural interface conformance.
  `semantic_async.inc` and `semantic_executor.inc` validate Future/JoinHandle/Executor operations, retained loans,
  concrete Send eligibility and consuming cancellation/shutdown paths.
- `src/ir` lowers typed AST nodes to explicit values and control flow, interns types, describes aggregate/enum layouts
  and imports. `ir_verify.c` verifies every use, definition, label, type, and symbol reference.
- `src/ir/ir_optimize.c` folds and propagates constants/copies, simplifies control flow and addresses, and removes dead
  values/stores/functions using CFG dataflow/liveness. It also reuses dominating pure calculations and fixed-array
  bounds checks, and hoists safe loop invariants. It preserves possible effects and traps, then verifies the resulting
  module. See [ARCHITECTURE.md](ARCHITECTURE.md#ir-optimization).
- `src/ir/ir_dump.c` serializes verified modules as versioned `dmm-ir-v6`.
- `src/backend/x86_64` consumes only verified IR. It owns stack layout, System V and Windows x64 calling conventions,
  scalar/SSE conversion, aggregate address calculation, runtime calls, and Intel/AT&T assembly formatting.
  `ir_arithmetic.c` lowers arithmetic and conversions; `ir_calls.c` lowers ABI calls and runtime calls;
  `ir_names.c` builds and validates native symbol names;
  `ir_output.c` writes assembly and native objects.
- `src/backend/asm_optimizer.c` performs the final conservative text cleanup.
- `src/backend/native/encoder.c` encodes structured instructions directly.
- `src/backend/native/object.c` owns sections, symbols and relocations and writes ELF64/COFF relocatable objects.
  `linker.c` lays out executable images, resolves relocations and emits static ELF segments or PE OS
  import/base-relocation tables.
- `src/runtime/native_runtime.c` supplies executable startup and native runtime shims; system imports have private names
  to prevent source-symbol collisions. See [ARCHITECTURE.md](ARCHITECTURE.md#native-x86-64-backend) for image layout and limits.
- `src/runtime/native_executor.inc` emits scheduling and Future adapters; `native_threads.inc` emits standalone
  threads/events. `executor.c` is the independently tested C contract implementation. `src/runtime/platform/*.dmm`
  supplies pthread/UCRT64 threads, events and exit through native exports; `network_shim.c` adds epoll/IOCP and bounded DNS.
  The compiler emits the platform `main` bridge. Runtime objects are built by the completed compiler in the
  `--runtime-component` bootstrap mode and then installed beside it.
- `src/diagnostics` buffers and renders text or JSON diagnostics from every phase.
- `src/driver` owns CLI validation and phase lifetime. `external_link.c` selects a matching GCC-compatible driver,
  the required private runtime shim and OS link dependencies, then checks external-link success.

## AST and semantic model

Every syntax node has a source span. Expressions retain their tree shape and evaluation order; postfix calls, indexes,
and members wrap their operand rather than reconstructing it later. Token spellings are interned once per complete
root/import graph and referenced by token index, reducing each token from a fixed maximum-sized text buffer to a stable
pointer.

Semantic symbols use stable IDs across the root program and all imported units. Global lookup uses a hash index keyed by
interned spelling and symbol kind; pointer equality is the common comparison path while public textual lookups remain
content-correct. Local symbols are scoped before IR lowering. Expression annotations record the primitive type, pointer
depth, named type symbol, array state, and referenced symbol. Backend emission therefore does not decide whether source
operations are legal.

Concrete struct and enum symbols also carry explicit `COPYABLE` or `MOVE_ONLY` ownership metadata plus the independent
`NEEDS_DROP` bit. Semantic analysis derives these properties to a fixed point from an explicit destructor and the
properties of concrete field and variant-payload types. Generic templates do not receive a single guessed classification: each specialized
aggregate is classified after type substitution. Move and borrow analysis query this metadata rather than rediscovering
ownership rules at individual expressions.

A dedicated ownership dataflow pass tracks live, moved, and uninitialized local owners. It merges branch and match-arm
states, validates loop backedges and `break`/`continue` exits, and applies concrete specialization properties to calls,
arrays, and enum construction. Interface values are move-only owners; erasure copies copyable implementers and moves
move-only implementers into their owned payload. Async ownership additionally requires consumption of Futures,
JoinHandles and Executors, preserving captured loans until completion or confirmed cancellation.

## Typed IR

IR types include primitive, named, pointer, fixed-array, slice, function, Future, JoinHandle and Executor types.
Instructions cover constants,
loads/declarations/stores, unary and binary operations, calls, indexes, members, slice construction/data/length, casts,
allocation/free, returns, branches, jumps, labels, and PHI values. Calls store a contiguous ordered argument slice, so
nested calls cannot corrupt argument ordering.

Async functions retain pinned-frame metadata, concrete Send properties and verified suspension states. Explicit
`await`, `executor`, `cancel-check`, `cancel-await`, `cancel-drop` and `cancel-return` instructions describe polling
and cancellation cleanup. Native lowering emits frame constructors, poll callbacks and cleanup callbacks; active
scope/drop flags preserve exactly-once cleanup across suspension. See [ARCHITECTURE.md](ARCHITECTURE.md#executor-runtime-and-native-abi).

Boolean `&&` and `||` lower to branch/jump/label/PHI control flow and therefore preserve short-circuit side effects.
Instance methods receive an explicit hidden aggregate pointer; implicit field names lower against that receiver. Slices
expand to data pointer and length at calls. Stored and returned slices use two-word aggregate descriptors; fixed-array
conversion lowers to explicit slice construction. Layout queries are folded during semantic analysis. `ir_type_layout`
supplies the backend's sizes, alignments and storage-slot counts, matching those query values. Typed allocation helpers
specialize ordinary DMM generic functions with explicit type arguments over byte primitives. Allocation uses a complete
type and releases are explicit. Stdlib output uses ordinary resolved overload calls.

The verifier rejects malformed type graphs, duplicate or missing value definitions, invalid symbol ownership,
nonexistent labels, ill-typed operations, calls and returns, and bad operand or argument references before backend
emission.

Every IR instruction retains its originating AST span. During x86 lowering, that span and its IR index are attached to
each structured machine instruction. The optional `dmm-source-map-v1` artifact records this mapping with source unit and
function names, and explicitly marks generated ABI, prologue, cleanup, and epilogue instructions as source-less.
`ARCHITECTURE.md` defines the stable serialization contracts.

## IR optimization

`-O1` is the default and enables AST and IR optimization. `-O0` preserves the typed AST for lowering and emits the
original lowered IR. The driver finishes semantic analysis and writes any requested AST dump before the AST pass, so
diagnostics and typed source dumps still cover the complete source tree. All output modes use the same IR module,
verified before and after IR optimization. Failure stops compilation. `--debug` reports transformation counts.
`--dump-ir-before-opt` captures IR after AST optimization and before the IR passes.

The typed AST pass folds decisive short circuits, selects `if` arms with known numeric or boolean truth, removes loops
with a known false condition while preserving `for` initializers, and cuts off statements after a guaranteed return,
break, or continue.
It changes function bodies only after semantic validation. Expressions with unknown truth or observable evaluation stay.
It also replaces a counted `while` loop with its final integer updates when the induction start, bound, step, and any
accumulated increment are compile-time constants. The body must contain only those local updates, and the induction and
combined increment must fit the `int` range. IR propagation can then reduce the result to one constant. Loops with calls,
unknown bounds, or potential induction overflow remain loops.

The pass repeats these transformations until stable:

- Constant folding of integer arithmetic, comparisons, logical operations, unary operations, casts and finite
  floating-point expressions.
- Constant and copy propagation through local storage and SSA operands. Forward dataflow meets predecessor facts at
  joins and converges across loops.
- Integer identities such as x+0, x-0, x *1, x/1, x-x and x*0; double negation and identity casts. Operand effects
  remain independently live.
- Constant branches become jumps; unreachable blocks disappear and PHIs with one remaining predecessor become their
  incoming value.
- Jumps through blocks containing only a label and jump are redirected when the destination has no PHI. Unreachable
  intermediate blocks then disappear in the control-flow pass.
- Common subexpressions reuse identical nontrapping computations and stable parameter loads in the same or dominated
  blocks. Fixed-array element values are still loaded independently; a repeated bounds check is omitted only when an
  equivalent access dominates it and its base and index are the same SSA values.
- Loop-invariant constants, stable parameter loads, and nontrapping calculations move to a unique preheader when every
  operand is available there. Address-taking, stores, and potentially trapping operations prevent the relevant move.
- Dead value elimination retains effectful or potentially trapping instructions.
- Hidden slice-backing release is an effectful memory operation and is never removed or reordered across calls.
- Backward CFG liveness removes dead and overwritten stores to unescaped locals. Declarations disappear only after every
  storage reference disappears.
- Address/dereference cancellation and repeated address/load simplification within a basic block and memory epoch.
- Unreachable private top-level functions are removed after call-graph traversal. Public functions, entry points, and
  methods remain available, including methods reached through interface dispatch.

Values, labels and call arguments are compacted so removed values no longer inflate stack frames. Source spans survive.
IR-synthesized numeric literals store bits in instructions rather than changing source tokens; `dmm-ir-v6` exposes them.
For optimized IR, x86 lowering fuses a signed integer comparison used only by the next branch, skips local loads used
solely as store targets, emits direct integer local updates, and lets a branch fall through to an adjacent true block.

### Semantic boundaries

Integer folding models 64-bit virtual arithmetic with unsigned wraparound, avoiding host signed-overflow undefined
behavior. Division by zero and INT64_MIN/-1 remain runtime operations. The pass preserves full local virtual slots and
typed-width indirect writes, including upper bytes of partial stores.

Floating folding preserves float/double precision and signed zero. Algebraic rewrites for unknown floating values are
excluded because NaN, infinity and signed zero invalidate integer identities. Non-finite arithmetic results and invalid
float-to-integer conversions remain backend operations.

Taking a local's address excludes it from storage dataflow and dead-store elimination while that escape remains in IR.
Indirect writes invalidate memory facts; calls, writes, allocation and release end load-reuse epochs. Heap/field stores
remain. Unused bounds checks and dereferences remain unless a specific rewrite proves an access redundant. There is no
whole-program alias analysis.

Aggregate ownership metadata is semantic input, not an optimization inference. Passes preserve
`COPYABLE`/`MOVE_ONLY`, the independent `NEEDS_DROP` bit, and explicit-destructor metadata. `drop`, `move`, and
`reinit` instructions are observable memory effects and may be removed or reordered only when ownership/liveness proves
that doing so preserves exactly-once destruction. The synthetic package-cleanup function is a liveness root even though
it has no source-level semantic symbol.

Async polling, executor operations and cancellation instructions are observable effects. The optimizer retains
`await`, `executor`, `cancel-check`, `cancel-await`, `cancel-drop` and `cancel-return`, remaps cancellation labels
during ID compaction and preserves the generated cleanup paths. The verifier checks the optimized module's
suspension and cancellation invariants before emission.

### Validation

`ir_optimizer_unit` checks transformed IR, CFG joins, loop backedges, PHI repair, copy/store elimination, pointer
aliasing, load reuse, overflow traps, bounds accesses, floating identities and idempotence.
`ir_optimization_execution` compares -O0 and -O1 executable output across the execution/regression corpus and verifies
unused divisions and out-of-bounds accesses still fail. Corpus tests also run optimized assembly, objects and internally
linked executables. IR fuzzing optimizes before verifier mutations.

## Storage and ownership lowering

Virtual values, parameters, and locals receive frame offsets before instructions are emitted. Large frames are probed a
page at a time. Fixed arrays and aggregate storage live directly in the frame and are copied by value; pointers and
scalar values use eight-byte virtual slots while indirect memory operations honor their actual element width.
Contextually typed array literals lower to `array-literal` IR with an explicit pattern and final element count. Fixed
arrays materialize inline. Slice literals materialize a two-word non-owning view plus hidden backing: local and returned
backings use heap storage with explicit `free-slice-backing` cleanup/ownership transfer, call temporaries live through
the call, and package literals reference static package-lifetime backing. The borrow pass records local slice copies as
storage-preserving views: replacing their backing owner is rejected until the copied view's conservative last use,
while indexed element access remains valid because it does not invalidate the backing address.

System V classifies integer and SSE arguments independently and spills overflow arguments in source order. Windows x64
uses positional registers and shadow space. Platform I/O shims live in the standalone runtime generator. Both targets
preserve stack alignment and the RBX callee-saved register used by address lowering. User functions that overlap the
runtime are mangled using canonical package identities; method symbols also encode their owning type. Public
declarations retain their DMM names in source; C callers use the emitted link names. Generic identities encode
declaration names and signatures.

`src/runtime/native_runtime.c` and `standalone.inc` generate machine instructions for strings, allocation, conversions,
fixed-format output, input and platform I/O. Executable packages embed these routines in assembly, objects and internal
executable images. Library objects have no executable startup requirement. Linux uses syscalls and static ELF startup;
Windows uses kernel32 APIs and its own file-descriptor table. The old C runtime archive has been removed.
Runtime requirements, runtime profile and link strategy are separate decisions. Used network intrinsics set `NETWORK`
and imply `PLATFORM_RUNTIME`; executable output then uses an external driver and the combined network shim. An unused
network import does not select that profile. `--link=external` can also select the smaller platform shim without
networking. Object/assembly output selects the same ABI without starting a linker. See
[stdlib/core/net/README.md](stdlib/core/net/README.md) for completion and startup/shutdown ordering.
`src/backend/runtime_calls.c` maps typed builtin calls to reserved runtime link symbols. The emitter performs ordinary
ABI argument/result lowering and contains no syscall-number selection, Windows file-flag mapping or input
implementations. The low-level core signatures in `src/common/core_intrinsics.h` are shared by semantic analysis, typed
IR verification and runtime call mapping. Core byte regions use `*u8`, counts use `usize`, and signed offsets/results
use `isize`.
`stdlib/core` exposes ordinary DMM wrappers; byte loads/stores, string traversal and fixed-width integer output are
implemented in DMM. See [stdlib/core/README.md](stdlib/core/README.md).

Runtime calls remain typed `IR_OP_CALL` instructions. Allocation/release and string concatenation call compiler-owned
routines; bounds checks remain backend operations.
`stdlib/io.dmm` builds higher-level I/O from typed runtime calls, while `print`/`println` are ordinary DMM
overloads. Raw allocation pointers are explicitly released. Concrete aggregate definitions carry
`COPYABLE`/`MOVE_ONLY`, `NEEDS_DROP`, and explicit-destructor metadata into verified IR. The verifier checks that
the ownership bits are coherent. Lowering emits explicit `drop`, `move`, and `reinit` ownership effects. Local
initialization flags make cleanup path-sensitive, and the existing cleanup stack emits drops on fallthrough, `return`,
`break`, and `continue`. Compiler-generated drop glue runs an explicit destructor body first and then recursively drops
owned fields and fixed-array elements in reverse declaration order. Enum drop glue tests the active tag and drops only
that variant's owned payloads in reverse payload order. By-value owning parameters use the same flags and
are destroyed by the callee. The main package receives synthetic package initialization and cleanup functions. IR
lowering orders the resolved package graph dependency-first with canonical-path tie-breaking, then emits runtime global
initializers before `main`. Cleanup drops initialized `NEEDS_DROP` globals and releases owned global slice backing in
reverse initialization order after `main`, while preserving its exit status. Package owners have companion
initialization flags that become live only after successful initialization, so reassignment and cleanup remain exactly
once.

`ir_verify_control_flow` builds basic blocks with explicit and fallthrough edges, computes entry reachability and
dominators, and checks that ordinary values are available at their uses. PHIs must begin a labeled join and name its two
actual jump predecessors; each value must be available on the corresponding edge. Instructions cannot follow a
terminator before a new label. Calls returning `never` have no SSA result and are followed by a defensive trap
terminator, making their non-fallthrough behavior explicit to CFG verification and optimization. Reachable non-void
exits require either a return or a terminating path; void bodies retain their implicit-return convention.

## Native x86-64 backend

Native FFI V1 adds separate `IrNativeImport` declarations (symbol identity,
ABI, logical library, native name, signature and source span) and native aggregate
layout metadata. These imports are not DMM function bodies. Sema uses the output
target before IR lowering; native size, alignment, offsets and array strides come
from a shared C-layout calculation, distinct from DMM's eight-byte aggregate slots.
The IR verifier checks native signatures, references and layouts. Unused native
declarations and compile-time layout queries can accompany ordinary programs.
Native calls use the separate classifier in `src/backend/x86_64/native_abi.inc`:
System V Eightbytes use INTEGER/SSE registers or stack memory with whole-aggregate
register rollback; Windows uses positional registers, shadow space and aligned
copies for indirect aggregates. Struct returns use registers or hidden result pointers.
`native-copy` IR captures by-value arguments before evaluation of later arguments.
Scalar return normalization ignores undefined upper register bits. Native copies and
field accesses use exact byte widths; native field arrays have C strides and native
values in ordinary DMM arrays retain rounded DMM slots. No DMM aggregate ABI rule is
used to classify a C call. Milestones are specified in [plans/ffi.md](plans/ffi.md).

Native callbacks use the same classifier and argument-placement routine for native indirect
calls and incoming exports. An export has a stable native symbol and a private
`__dmm_export_body_SYMBOL` DMM implementation. The entry captures register/stack
arguments, reconstructs aggregates, calls the private body and converts the result
to the native ABI. Function signature interning and mangling distinguish native and
DMM callable types. Optimizers and emission selection retain native exports.

Native unions, packing and explicit alignment are included in semantic and verified
IR layout metadata. Native values with alignment 16 remain aligned in stack slots,
parameter/result copies, global storage, ordinary struct fields and enum payloads.
Windows platform synchronous bodies and native entries emit `.pdata`/`.xdata` with
image-relative COFF relocations and four-byte section alignment. Frame-pointer unwind
records also cover outgoing call stack adjustments. Large frames use MinGW's stack
probe helper. Assembly output emits corresponding `.seh_*` directives. The records
support OS stack inspection; foreign exception propagation remains unsupported.

Raw bindings in `stdlib/native` describe libc/pthreads, Kernel32, UCRT and Winsock,
including epoll's packed event, OVERLAPPED unions and resolver structures. Target
source suffixes are selected before package parsing using the output target.
The platform runtime is implemented in DMM; the network implementation remains in C until migration stage 5.

Used native imports select the platform profile and external linking in `auto` mode.
The driver collects imports from the selected emitted functions, deduplicates logical
libraries and creates a dynamic argv without a shell. `--native-library NAME=PATH`
overrides one logical ID; repeatable `--native-library-dir DIR` adds search paths.
Overrides remain file arguments, and logical IDs cannot contain linker options.
Object and assembly emission require no library file or external process.
`--dump-native-link FILE` writes a versioned target/profile/import inventory.
For example:

```sh
compiler program.dmm --native-library sample=/path/libsample.a -o program
compiler -c program.dmm --dump-native-link program.link -o program.o
gcc -no-pie -pthread program.o /path/dmm-runtime/elf/platform-shim.o -lsample -o program
```

The compiler emits machine code directly from verified IR and structured x86-64 instructions. Native compilation invokes
no assembler, C compiler or external linker in its default standalone profile. Standalone output embeds a compiler-owned runtime: generated programs
require no libc, Windows CRT or foreign language runtime. The compiler itself remains implemented in C.

### Private platform-runtime handoff

`--link=auto|internal|external` selects the link strategy (default `auto`). Runtime requirements are separate from
link strategy: required IR operations determine a `standalone` or `platform` profile, and the driver resolves a
supported linker for that profile. Used network IR requires `NETWORK`, which implies `PLATFORM_RUNTIME`; neither an
unused import nor the `async` manifest feature requires a platform runtime. The networking ABI, combined shim and
typed Core API are specified in [stdlib/core/net/README.md](stdlib/core/net/README.md).

Explicit `--link=external` requests `PLATFORM_RUNTIME`. `auto` selects the
internal linker for standalone output and the external driver for platform output. `internal` cannot satisfy a
platform requirement. With `--emit=obj` or `--emit=asm`, `--link` selects the runtime/ABI profile intended for later
linking; **no link process starts**, and linker-driver/shim overrides are not needed or consulted.

```sh
compiler --link=external program.dmm -o program
compiler --link=external --linker-driver /usr/bin/clang program.dmm -o program
compiler --link=external --emit=obj program.dmm -o program.o
compiler --link=external --emit=asm --syntax=att program.dmm -o program.s
```

Platform executables use the platform's C startup and documented platform libraries. Standalone startup, native
runtime and internal linking are unchanged. Platform output still embeds the generated DMM memory/I/O runtime and
scheduler, but leaves private thread/event operations to a separately compiled DMM component. It defines
`int __dmm_runtime_main(void)` instead of `__dmm_entry`; DMM `main` is privately named `__dmm_program_main`.
The compiler also emits the native `main` bridge to `__dmm_runtime_main()`; no custom C entry is linked.

The generated bridge owns the lifecycle: loader-initialized runtime storage is available before package initializers;
then package initialization, DMM main, preservation of its exit code, default-executor drain, package cleanup and
return to C startup. Draining releases the default executor's runtime state. No additional eager runtime allocation
or global teardown phase is introduced. A void main returns zero. Immediate `exit`, traps and fatal failures bypass
normal DMM cleanup; platform `exit` terminates the entire process, including when called from a worker.
The DMM platform component has no package-cleanup or scheduler-lifecycle knowledge. Its private ABI is documented in
`src/runtime/platform_shim.h` and `ARCHITECTURE.md`.

The external executable path defaults to `gcc` from PATH. `--linker-driver PATH` selects a GCC-compatible driver;
`--runtime-shim PATH` selects a matching private runtime object or archive. CMake first builds the compiler, then
uses it to build `src/runtime/platform/*.dmm` and installs the object next to the
compiler as `dmm-runtime/elf/platform-shim.o` or `dmm-runtime/coff/platform-shim.o`. Discovery uses the running
compiler's directory, including when the compiler was found through PATH. Linux GCC/Clang and Windows MinGW-w64
UCRT64 GCC/Clang are supported; MSVC and MSVCRT shims are not supported. Cross-linking requires both overrides and
a matching target toolchain. The override is a private ABI implementation, not a public arbitrary-object or FFI API.
Network requirements instead select the complete `network-shim.a` in the same directory, add Windows `ws2_32`,
and insert DRAINING before package cleanup and network shutdown afterward. Pure platform programs retain the smaller
component and no networking dependency. The network archive contains the DMM platform object and the remaining
network C object; GCC and Clang can link it without a relocatable COFF linker. Archive overrides are checked for
the correct machine and object format in every object member.

`--runtime-component` defaults to object emission and also accepts `-S`. It requires a library package and
rejects executable emission, link-mode/driver/runtime overrides, runtime global initializers, owned global cleanup,
reachable async/drop bodies and implicit runtime-helper dependencies. Its only exported symbols are the fixed
private platform entries plus `__dmm_platform_thread_entry`; ordinary helpers and imported DMM functions remain
local. Native imports (including explicitly declared base helpers, if needed) remain unresolved and are recorded
by `--dump-native-link`. The compiler never attaches application startup, scheduler or platform/network objects
to a component. Windows compiler stack probes remain a driver-provided dependency.

Build the default CMake target (or `dmm_platform_runtime` / `dmm_network_runtime`) before installation. Runtime
objects depend on the compiler and DMM binding sources, preventing a bootstrap cycle and rebuilding after changes.

Manual linking of platform output uses regular C startup, **without** standalone entry flags or `-nostdlib`:

```sh
# Linux; substitute program.s for program.o to link assembly output.
gcc -no-pie -pthread program.o /path/to/dmm-runtime/elf/platform-shim.o -o program
# Windows UCRT64; substitute program.s for program.obj for assembly output.
gcc program.obj C:/path/to/dmm-runtime/coff/platform-shim.o -lkernel32 -Wl,--subsystem,console -o program.exe
```

Linux platform output is non-PIE, consistent with the native object's absolute data relocations. The driver invokes
the external tool with individual arguments and no shell, captures stdout/stderr and status in text/JSON diagnostics,
and publishes a temporary linked image only after success. A failed link preserves an existing requested output.
Only platform programs gain libc/UCRT and the required OS dependencies; network libraries are deferred until network
operations exist. Reproducible external linking is scoped to a fixed toolchain, shim and linker configuration.

```sh
compiler --emit=exe --target=elf program.dmm -o program
compiler --emit=exe --target=coff program.dmm -o program.exe
compiler --emit=obj --target=elf program.dmm -o program.o
compiler --emit=obj --target=coff program.dmm -o program.obj
```

Executable output (`--emit=exe`) is the default; the target defaults to the host. Use `-c` / `--emit=obj` for objects or
`-S` / `--emit=asm` for assembly. Default filenames append `.out` / `.exe`, `.o` / `.obj`, or `.s` to the source. Both
targets can be emitted from either host. Syntax selection affects assembly printing, not native bytes. Native headers
and symbol ordering are deterministic.

### Objects and assembly

Objects are ELF64 ET_REL or AMD64 COFF with text, read-only data, writable data, symbols and relocations. References use
ELF PC32/PLT32/64 or COFF REL32/ADDR64. Assembly embeds the same runtime instructions as byte directives and symbolic
relocations. No separate runtime archive or target C toolchain is needed to generate either format.

To link an object or assembly file with GNU tools, supply the own entry point:

```sh
# Linux
gcc -nostdlib -no-pie -Wl,-e,__dmm_entry program.o -o program
# Windows / MinGW
gcc -nostdlib -Wl,--entry=__dmm_entry,--subsystem,console program.obj -lkernel32 -o program.exe
```

The same commands accept generated `.s` files. C ABI interoperability tests deliberately link a C caller with its host
runtime; this is optional and does not make libc/CRT a requirement for DMM output.

### Internal executables

The internal linker combines the generated module and built-in runtime, resolves symbols/relocations, and constructs
executable headers. It does not accept arbitrary external objects or archives.

Linux output is static ELF64 ET_EXEC at base `0x400000`, with separate code, read-only and writable load segments and a
non-executable stack. There is no PT_INTERP, PT_DYNAMIC or dynamic library dependency. Startup calls DMM main and exits
with syscall 60. Allocation and file/console I/O use Linux syscalls.

Windows output is a PE32+ console image. Only `kernel32.dll` is imported:
VirtualAlloc, VirtualFree, GetStdHandle, ReadFile, WriteFile, CreateFileA, CloseHandle, GetLastError and ExitProcess.
Startup calls package initialization, then `main`, then normal package cleanup and ExitProcess. Absolute data pointers receive DIR64 base relocations; internally linked images
enable ASLR and NX.

Runtime generators live in `src/runtime/native_runtime.c` and `standalone.inc`, outside the IR emitter. They implement
string length/comparison/copy/append, duplication, allocation/release, decimal integer parsing and formatting, fixed
six-decimal binary64 formatting, bounded output, scalar input and file I/O. Binary64 formatting rounds ties to even and
handles subnormals, infinity, NaN and signed zero without libc. Private runtime symbols prevent source functions such as
write from intercepting platform calls. Unknown executable imports fail. Runtime operations remain typed, verified IR
calls.

Concrete aggregate ownership metadata reaches verified IR. The native backend lowers explicit `drop`, `move`, and `reinit`
effects, keeps initialization flags for `NEEDS_DROP` locals, and calls compiler-generated drop glue. Drop glue executes
the user destructor before recursively destroying owned fields and fixed-array elements in reverse order. Sum-enum
drop glue dispatches on the active tag and destroys its owned payloads in reverse order. By-value
owning parameters are cleaned up by the callee. Runtime package initializers are lowered into
`__dmm_package_init` in deterministic dependency-first order. `NEEDS_DROP` package globals have private initialization
flags and are dropped by `__dmm_package_cleanup` in reverse initialization order; owned global slice backing is released
there as well. Native startup calls initialization before `main`, invokes cleanup after a normal return, and exits with
the preserved return value; the immediate `exit` intrinsic bypasses cleanup.

`array-literal` materializes fixed arrays inline and fills slice backing by cyclically repeating its typed pattern.
Local slice backing is released by explicit `free-slice-backing` effects at the owning scope exit; returned backing is
transferred to the caller's receiving lvalue, and temporary call arguments are released only after the call. Static
package slice literals point at static data. Runtime-created backing transferred into package storage is tracked by a
private owner slot and released on reassignment or normal package cleanup.

### Validation and limits

CTest executes the full language corpus through internal executables and assembly/object links with `-nostdlib`.
Dependency checks forbid dynamic ELF loading and foreign Windows DLLs. Unit tests execute emitted runtime instructions,
compare 2,000 finite binary64 formats with a test-only reference, and cover allocation overflow, integer limits, file
operations and negative errors. Other checks cover C ABI interoperability, EOF/input/truncation, deterministic bytes,
cross-format headers/relocations and artifact/source protection.

Local Linux GCC and Windows UCRT64 GCC validation pass 41/41 suites; Windows UCRT64 Clang also passes the networking
object/assembly matrix. Linux ASan/UBSan checks the network shim and test harness with leak detection. Sanitizers
instrument C code, not emitted instructions. Hosted CI results require separate verification.

The allocator maps each allocation separately. Windows maintains 253 file slots plus standard descriptors; input
strings have a 255-byte limit. The internal formatter implements the fixed formats needed by DMM operations, not a
general C printf API. Internally linked standalone images lack DWARF/PDB and Windows unwind tables. Native source maps contain text-section
offsets for source instructions.

Format references: [PE/COFF specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format),
[ELF program headers](https://refspecs.linuxfoundation.org/elf/gabi4+/ch5.pheader.html)
and [Intel instruction manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html).

## Executor runtime and native ABI

### Private platform ABI v1

The optional platform profile keeps the emitted scheduler but implements thread/event primitives in
`platform/*.dmm`. The compatibility declarations in `platform_shim.h` use the x86-64 System V or Microsoft x64 C ABI. Thread
creation takes `void (*callback)(void *)` and its argument and returns a non-null opaque handle. The callback returns
normally. Join waits for confirmed thread termination, then consumes the handle. Allocations never cross runtime
ownership boundaries. Resource/API failures are fatal, with no recoverable partial startup contract.

Linux implements threads with pthreads, including libc TLS initialization. Windows is specifically MinGW-w64
UCRT64: `_beginthreadex` starts a native DMM export, normal callback return performs CRT thread cleanup, and join uses
`WaitForSingleObject` followed by `CloseHandle`. Generated workers do not use raw clone or CreateThread in this profile.

`__dmm_async_wait_create` returns an initially unsignalled manual-reset event. `__dmm_async_wake` signals it and
retains that signal until `__dmm_async_wait_reset`; wait does not consume the signal. Linux uses a mutex-protected
predicate and condition-variable loop; Windows uses a manual-reset Win32 event. Reset remains under the scheduler
predicate lock, and destroy requires every waiter to have stopped. Existing no-lost-wake and single-poller invariants
apply identically to standalone and platform profiles.

Regular platform C startup calls the compiler-generated `main`, which forwards to `int __dmm_runtime_main(void)` once.
The generated object owns package init, DMM main, default-executor drain, package cleanup and the preserved return status.
Immediate process exit and traps do not run this normal cleanup path. `__dmm_platform_exit` terminates the whole
process from any thread. This private ABI is not a source-language foreign-function interface.
With NETWORK, `network-shim.a` combines the remaining network C implementation and the DMM platform object,
and supplies reactor/DNS operations through the generated I/O acknowledgement ABI.
Networking remains RUNNING throughout executor Drain; it enters DRAINING immediately before package cleanup and
shuts down after cleanup. The core ownership contract and platform implementations are in
[stdlib/core/net/README.md](stdlib/core/net/README.md).

`executor.h` defines the internal C scheduler contract used by
`executor_runtime_unit`. The standalone backends emit the equivalent scheduler
in `native_executor.inc` and platform primitives in `native_threads.inc`.
The C contract implementation is not linked into standalone DMM executables.
The frontend enforces the public API and ownership; IR and native callbacks
implement cancellation and active-scope cleanup. See [LANGUAGE_SPEC.md](LANGUAGE_SPEC.md) for
the source contract.

### Emitted frame ABI

Frames remain pinned. The private header contains poll at byte 0, destroy at 8,
state at 16, cancellation poll at 24, result destructor at 32, poll context at
40, result-present at 48, cancellation state at 56, auxiliary storage at 64,
and result-cancellation callback at 72. Results start at 80. A JoinHandle uses
its result tag at 80 and payload at 88; its auxiliary field owns the task until
join consumption, and byte 56 records the output byte count. Callbacks receive
`(frame, context)`; destroy receives `(frame, discardResult)`.

Context addresses identify retained tasks. Bit 0 suppresses cancellation during
an active defer cleanup; retain, release and wake mask that tag. Result cancellation
walks owned child slots to acknowledged completion, clearing consumed slots so
repeated Pending polls cannot duplicate destruction. Result destruction runs only
after that walk finishes. The ABI is private and not a public foreign-function API.

### Synchronization and ownership

The executor mutex protects admission, active tasks, the ready queue, polling
state, terminal status, cancellation requests and waiter registration. Callbacks
run outside this lock. Queue membership and a notification received during a poll
are separate bits. A worker clears the notification only when starting a poll;
a subsequent wake survives Pending and queues another poll. Running frames cannot
be queued. Pending without a notification parks until a wake or cancel request.

The scheduler and consuming handle each own a task reference. Cloned wakers own
additional references. Each task owns an executor reference, so consumed executor
owners do not invalidate joins or wakers. Terminal publication occurs after any
cancellation destruction. Scheduler ownership ends after waiter delivery. Ready
results remain in their frames until join consumption or explicit cancellation.
Callbacks move outputs through `take_result`; `destroy(frame, true)` discards an
unclaimed completed output. Cancellation cleanup must destroy captured resources
but must not claim to have produced an output.

`dmm_join_cancel` consumes a join and transfers its reference into a cancellation
operation. Callers poll that operation and finish it only after termination. No
loan or resource may be released just because a request was made. The normal
Ready/cancellation winner is selected under the executor lock after poll returns.
A request that won during a successful normal poll discards that poll's output.
Shutdown Cancel leaves already published Ready results available to their joins.

`dmm_poll_cancel_requested` observes requests made during a running callback. The
future's implementation must use it at cooperative suspension boundaries. The
scheduler never interrupts synchronous code. Cancellation callbacks can suspend
using the same waker while child futures, I/O and asynchronous cleanups finish.
The compiler remains responsible for active-scope tracking and cleanup order.

Consuming Future adapters are provided for direct future cancellation, join
await and shutdown await. Cancelling a cancellation or shutdown adapter continues
its underlying cleanup rather than abandoning it. The join adapter preserves the
Ready/Cancelled result and retains unclaimed output ownership until its result
is consumed; cancellation discards such an output through the ordinary join path.

Waiter wakers are replaced and delivered without holding executor locks across
executors. The I/O adapter separates request from confirmation and synchronizes
confirmation with destruction. Frame and confirmation adapter own independent
references; confirmation retains its reference through wake delivery even when
the resumed frame concurrently consumes its ownership. The adapter must not
access an operation after confirmation or leave other adapter calls in flight.

### Standalone native primitives

The emitted `__dmm_async_thread_create(callback, argument)` and
`__dmm_async_thread_join(handle)` use CreateThread/WaitForSingleObject on COFF and
raw clone/exit/futex on ELF. Linux native workers have no libc TLS and must execute
only generated code or callbacks that do not require libc TLS. Their stack remains
allocated until the kernel clears the child TID, then join frees the stack and
control object. Joining consumes the handle.

`__dmm_async_wait_create`, `__dmm_async_wait`, `__dmm_async_wake`,
`__dmm_async_wait_reset`, and `__dmm_async_wait_destroy` implement manual-reset
events using Win32 events or an atomic word plus private futex. A signal preceding
a wait is retained. Reset must occur under the scheduler predicate lock before
parking; destruction requires that all waiters have stopped. Resource failures
trap. Thread primitives are encoded directly for object, assembly and executable
outputs through the existing runtime emitter.

### Verification

`executor_runtime_unit` forces parallel polling, concurrent wakes during Pending,
self-wakes, cancellation before first poll, cancellation during running Ready and
Pending polls, completed-result cancellation, nested cross-executor awaits and
cancellation, delayed child I/O confirmation, asynchronous cleanup confirmation,
shutdown with open joins, caller-thread blocking and default executor use.
Future adapters cover void cancellation, cancelled join results, discarded join
outputs and shutdown from another executor, including cancellation of shutdown.

`runtime_unit` executes emitted scheduling, thread, event and private I/O
instructions on the host OS, including parallel polls, wake races, running-poll
cancellation and shutdown with delayed confirmation and open joins.
`async_runtime_contract` exercises generated cancellation before first poll,
Pending cancellation, awaitable defer cleanup, result disposal and asynchronous
child-result disposal. `async_executor_contract` compiles the public API to
standalone ELF and COFF at O0/O1 and executes the host format.
`async_semantic_unit` checks ownership, concrete Send eligibility, loans and
negative cancellation verifier transitions. `native_binary_unit` verifies
both ELF and COFF encoding/linking.

## Compiler debugging dumps

The compiler exposes deterministic, line-oriented debugging formats. Each begins with a version marker; consumers must
reject unknown versions rather than infer a schema from individual fields.

```sh
compiler -S --dump-tokens program.tokens --dump-ast program.ast --dump-symbols program.symbols \
  --dump-ir-before-opt program.lowered.ir --dump-ir program.ir --dump-cfg program.cfg \
  --source-map program.map -o program.s program.dmm
```

Paths must be distinct from all loaded sources, generated output, and one another. Dump creation is part of compilation:
an I/O or validation failure makes the command fail and removes the incomplete artifact.

Dumps are written as soon as their compiler phase has completed. Consequently, `--dump-tokens` remains available after
a parser failure, and `--dump-ast`/`--dump-symbols` remain available after a semantic failure. IR and CFG dumps require
successful semantic analysis and lowering.

### `dmm-native-link-v2`

`--dump-native-link FILE` describes the output target ABI and runtime profile after
function selection, then lists native imports with logical library IDs and native
symbol names. Explicit overrides add their file path; requested search directories
are listed separately. Unused imports are absent. Quoted values escape quotes,
backslashes and newlines. This inventory accompanies object/assembly output without
starting a linker. Platform output still requires the matching installed runtime
component and regular C startup, as described in [ARCHITECTURE.md](ARCHITECTURE.md#native-x86-64-backend).
Version 2 adds `runtime-profile=component` for `--runtime-component`: these objects
have no application startup or automatic runtime dependencies. Other profile values
remain `standalone` and `platform`. Global native function-pointer initializers also
contribute used imports.

The IR instruction inventory also includes `native-copy`, a byte-exact snapshot
of a native struct before evaluation of later call arguments.

### IDE analysis mode

`--ide --dump-ast FILE` performs editor analysis without code generation. It recovers from syntax errors where possible
and writes a validated partial AST with semantic information; malformed declarations may be omitted, and lexer failures
can prevent a dump. `--ide-buffer FILE` reads the root document from an editor snapshot while retaining the original
source path for imports, identities and diagnostics. Imported packages are still read from disk. IDE mode cannot be
combined with program emission, IR, CFG, source-map or token output.

### `dmm-native-map-v1`

Native `--emit=obj` and `--emit=exe` source maps begin with this marker. Records use the assembly map's instruction,
function, source, IR index and span fields, prefixed with `text-offset N`, the instruction's byte offset in the text
section. Executable offsets cover source-generated code before runtime/startup/import thunks are appended. They are
section offsets, not file offsets or addresses.

### `dmm-tokens-v1`

The token dump inventories the root source and all resolved import units. Every token has its stable unit-local index,
token kind, exact escaped source spelling, and begin/end source position. It is useful for diagnosing keyword
classification, escaped literals, source coordinates, and import-specific lexer behavior without enabling noisy
terminal debugging.

### `dmm-ast-v6`

`struct-literal` expressions have an `operand-type` and ordered `initializer-field` name/symbol records.
Their `argument` children preserve field-value expressions in source order. Field labels retain their own token identity
and never replace a value expression's identifier or literal token.

Native declarations include `native=1`, `opaque=...`, `abi=...`, optional `library=...` and
`native-name=...` on native declarations. Extern blocks flatten into package
declarations while ABI/library/alias token fields retain their source coordinates.
Native structs never carry a library field. Native imports have no body.

The AST dump lists the root and every loaded import unit, each as a preorder traversal. Two-space indentation records
ownership; the explicit role following `statement` or `expression` records the child edge. Declarations, parameters,
fields, enum variants, statements, expressions, resolved types, stable semantic symbol IDs, operators, folded constants,
type operands, and source spans are included. A dash denotes an absent optional value.

Generic parameter/bound records and specialization identities are explicit. Type arguments appear recursively in type
spellings. Interface and struct methods retain their source ownership; enum payload and match-arm/binding records
describe those nodes. Template declarations remain visible in AST dumps but are absent from IR. Specialized
declarations include `generic-origin name="..."` and ordered
`type-argument type=...` children so consumers can display source-level generic types instead of private specialization
identifiers. Match bindings include their semantic symbol ID and the name token's source span.

Compile-time metadata adds `type-info` expressions with `operand-type` and
`type-property` expressions with a `folded` value. Type-match patterns include
`type=...`; the chosen arm is marked `selected`. Metadata and discarded type arms do not generate runtime IR values or
code.

Async function declarations include `async=1`. `Future<T>` and `JoinHandle<T>` retain their output type in type
spellings; consuming `.await()` appears as an `await` expression.

### `dmm-symbols-v1`

The symbol dump lists every semantic symbol by stable ID and declaration order. Records include kind, spelling, source
unit, token, owner, scope depth, resolved primitive/named type, pointer depth, array/slice flags, and whether a source
declaration owns the symbol. Concrete struct and enum records also include a `properties=` field containing `COPYABLE` or
`MOVE_ONLY` and, independently, `NEEDS_DROP` when applicable. Generic templates are classified through each concrete
specialization rather than receiving one template-wide property set. The header also reports symbol-index capacity and
occupancy, unresolved expressions, duplicates, and semantic errors. Hash slots are intentionally omitted because their
placement is an implementation detail and may depend on process addresses.

### `dmm-ir-v6`

`struct-literal` produces fresh zeroed aggregate storage. Ordered `member`/`store` instructions fill
the fields using their resolved identities and target layouts; destruction of incomplete owned fields is explicit in IR.

The module header includes `target=elf|coff` and `native-imports=N`.
Each native import is a separate `native-import #N symbol=... abi=... library=...
name=... return=@N parameters=[...] span=...` record. It never has an IR function
body. Native aggregates add `native opaque=... size=... alignment=...` followed
by `native-field #N offset=... array-stride=...` records. Zero stride denotes a
non-array field. Opaque native structs have zero size/alignment metadata and
cannot be used as complete values. Ordinary aggregate fields retain their slot
representation; native offsets are in bytes.

The IR dump lists interned types and aggregate definitions before functions. In-memory aggregate records retain
the derived ownership properties and whether an explicit destructor exists; these fields drive verification and native
drop-glue generation. `dmm-ir-v6` currently serializes aggregate names, symbols, and fields, but not
those ownership fields; exposing them requires a new dump-format version. Function instructions are numbered in storage
order. Values use `%N`, types use
`@N`, and control-flow labels use `LN`. Each instruction records its opcode, result, type, operands, symbol/token
references, argument slice, operator, and source span. Contextual literals appear as `array-literal` with their pattern
arguments and final element count. Ownership cleanup appears as effectful `drop`, `move`, `reinit`, and
`free-slice-backing` instructions, including inside compiler-generated cleanup/drop-glue functions. Executable main packages also contain
a synthetic package-cleanup function whose reverse-order `drop` effects target `NEEDS_DROP` globals. Enum payload
constants and imported-unit references are explicit.
Numeric constants produced by optimization include `immediate=0x...` with their 64-bit integer value or IEEE
floating-point bits. A fixed-array index may carry `bounds-check=elided` when an earlier dominating access checked the
same SSA base and index. Original source tokens remain unchanged. Dumps describe optimized IR by default; use `-O0` for the
lowered IR. `--dump-ir-before-opt` always captures IR immediately after lowering, so it can be diffed against
`--dump-ir` to explain a transformation. Optimization compacts value and label IDs while preserving source spans.

The current `dmm-ir-v6` writer also emits `future output=@N pinned=1`, `join output=@N pinned=1` and `Executor`
type records. Each async function has an `async constructor=1 poll=1 cleanup=1 pinned=... states=... future=@N send=... sync=0`
metadata line. Instructions include `await`, `executor`, `cancel-check`, `cancel-await`, `cancel-drop` and
`cancel-return`. The dump does not serialize every private in-memory frame/cancellation field or runtime requirement;
see the verifier/runtime contracts for those details. Consumers must support the emitted async records explicitly.

### `dmm-cfg-v1`

The control-flow dump groups the selected IR instructions into basic blocks for each function (`-O0` keeps the lowered
graph). Blocks record reachability,
half-open instruction ranges, predecessor and successor sets, followed by their instruction opcode, result value, and
source span. This is the same CFG construction consumed by verification and optimization, making it suitable for
debugging malformed edges, unreachable code, PHI placement, and branch folding.

### `dmm-source-map-v1`

The source map contains one entry per structured x86-64 instruction emitted by the backend, independent of Intel or AT&T
printing. `instruction N` is the logical instruction ordinal before the conservative assembly cleanup pass. Each entry
names its source-level function and source file. Instructions lowered from IR additionally carry `ir=N` and a 1-based,
end-exclusive source span; compiler-generated prologue, epilogue, cleanup, and ABI instructions use
`ir=- span=-`.

Quoted values use `\\`, `\"`, `\n`, `\r`, and `\t` escapes. Other ASCII control bytes use `\xNN`. Field order, traversal
order, identifier numbering, and final newlines are part of each versioned format. Adding or reordering fields requires
a new format version.

Native AST declarations carry export ABI and union/pack/align attributes. Native callable spellings distinguish
`extern system func`. IR function types print `extern system function`, exports have a `native-export` record, and
aggregates include `native-layout union=... pack=... explicit-align=...`. The verifier recomputes layouts and checks
these attributes. Emitted global native function addresses also contribute library requirements to the native-link
inventory.

## Compiler diagnostics

The numeric constants in [`src/diagnostics/errorHandler.h`](src/diagnostics/errorHandler.h) are the authoritative diagnostic code inventory.

### Diagnostic families

| Prefix | Category | Examples |
|---|---|---|
| `L` | lexer | malformed literals, escapes, UTF-8 and source reads |
| `P` | parser | unexpected or missing tokens and invalid declarations |
| `T` | type system | unknown types, invalid operations and incompatible values |
| `S` | semantic analysis | resolution, control flow, ownership, borrows, packages and modules |
| `G` | code generation | output and target-emission failures |
| `C` | compiler driver | entry point, options, imports, dumps and internal failures |
| `W` | warnings | reserved warning IDs; warning policy is still roadmap work |

Numeric values are stable identifiers within a category, not a global sequence. A code must be paired with its category
prefix. Package and module diagnostics occupy the semantic `S120`–`S130` range.

### Rendering contract

Human diagnostics contain the category/code, message, source path and, when available, a source line with a one-based
start and end-exclusive underline. Tabs count as one source column; Unicode columns count code points. Diagnostics with
no source span still identify the relevant path or compiler operation.

`--formatError` emits one JSON document with an `errors` array and a `summary` object. Each diagnostic can contain:

- `errorCode`, `category`, `severity` and `message`
- `filename`, `line`, `column`, `endLine` and `endColumn`
- `sourceLine`, `token` and `suggestion`
- related diagnostics in `children`
- either a machine-applicable `fix` or `null`

Coordinates are one-based and end-exclusive; zero means unavailable. Invalid source bytes are replaced safely during
JSON serialization. Human output displays at most ten errors and reports omissions, while JSON retains the complete
buffer and records `summary.suppressedErrorCount`.

Fixes are suggestions and are never applied by the compiler. They are emitted only when the replacement and source span
are unambiguous. Missing semicolons can suggest insertion; a method reference can suggest `()` only for a valid,
unambiguous zero-argument non-void method.

### Diagnostic quality rules

- Report the source cause once; suppress dependent conversion or backend noise.
- Underline the supplied value for initializer, assignment, argument, return and enum-payload conversion failures.
- Attach the previous declaration or opening delimiter as a related location when useful.
- Preserve the real failing source, import, output, dump or source-map path and include the operating-system reason.
- Distinguish ownership failures such as use after move, partial moves, loop-carried consumption, escaping borrows,
  package-owner moves and move-only interface erasure.
- Treat internal lowering, verification and emission failures as compiler diagnostics with function, instruction, type
  and source-span context rather than assertions visible to users.

### Validation

The following CTest targets cover rejection fixtures, runtime failures and diagnostic rendering:

- `diagnostics_audit` runs every rejection fixture in human and JSON modes, checks counts and validates source spans.
- `diagnostics_cases` checks exact codes, ranges, messages, related locations, output paths and error limits.
- `diagnostics_json` covers fixes, ambiguity suppression, imports, Unicode, CRLF, EOF and multiple buffered errors.
- `lexer_unit` covers invalid UTF-8 serialization, Unicode columns, literal recovery and escaping.
- `resource_fault_unit` injects allocation and file-operation failures through the frontend, semantic model, IR,
  native object emission and output cleanup paths.
- parser, semantic and IR fuzz targets exercise malformed inputs without bypassing the normal diagnostic handler.

Run the focused suite with:

```sh
ctest --test-dir build -R "diagnostics_(audit|cases|json)" --output-on-failure
```

The audit demonstrates coverage of the checked corpus and injected failures; it is not a proof that every hostile input
or operating-system failure produces an ideal diagnostic.

## Tests

The regression corpus assembles and runs every positive program in Intel and AT&T syntax. Separate suites cover C ABI
interoperability, rejection diagnostics, runtime bounds traps, imports and cycles, CLI behavior, architecture
boundaries, large dynamic compiler state, AST construction, IR verification, optimization, and lexer/syntax conversion
behavior. Parser, semantic, and IR libFuzzer targets reuse the in-memory frontend API; the IR target also mutates safe,
non-owning instruction fields to exercise malformed-module verification.
Dedicated async, executor, platform-handoff and network suites check manifest gating, ownership, pinned frames,
ELF/COFF emission at O0/O1, scheduling/wake races, cancellation confirmation, loopback TCP/UDP/DNS and lifecycle.
