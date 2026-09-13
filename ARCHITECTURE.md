# Compiler architecture

## Pipeline

The compiler has a single production pipeline:

```text
source file
  -> token stream
  -> arena-owned structured AST
  -> semantic model and typed AST annotations
  -> verified target-neutral IR
  -> IR peephole/dataflow optimization and verification
  -> target-aware structured x86-64 instructions
  -> assembly printing/cleanup OR direct encoding
  -> ELF/COFF object serialization OR internal ELF/PE executable linking
```

Frontend or semantic errors stop compilation before IR creation. IR verification
failures are compiler errors. The backend never reparses source tokens and there
is no alternate compatibility emitter.

## Ownership

- `src/frontend/lexer.c` owns lexical analysis and token diagnostics.
- `src/frontend/syntax_parser.c` constructs all declaration, type, statement,
  and expression nodes in the AST arena.
- `src/frontend/frontend.c` owns file/import loading. Quoted imports are relative
  to their source unit; angle imports can resolve from the bundled library root.
- `src/common/string_interner.c` owns the module-wide canonical spelling table.
  Root files and imports share it, so equal source strings have pointer identity.
- `src/ast` owns program lifetime, the shared string interner, spans, and AST storage.
- `src/ast/ast_dump.c` serializes the resolved tree as versioned `dmm-ast-v2`.
- `src/sema` collects global/member/local symbols, resolves expressions and
  named types, validates scopes, calls, conversions, lvalues, returns, bounds,
  and control-flow placement, and annotates AST nodes with stable IDs and types.
- `src/ir` lowers typed AST nodes to explicit values and control flow, interns
  types, describes aggregate/enum layouts and imports, and verifies every use,
  definition, label, type, and symbol reference.
- `src/ir/ir_optimize.c` folds and propagates constants/copies, simplifies control
  flow and addresses, and removes dead values/stores using CFG dataflow/liveness.
  It preserves possible effects and traps, then verifies the resulting module.
  See [IR_OPTIMIZATION.md](IR_OPTIMIZATION.md).
- `src/ir/ir_dump.c` serializes verified modules as versioned `dmm-ir-v3`.
- `src/backend/x86_64` consumes only verified IR. It owns stack layout, System V
  and Windows x64 calling conventions, scalar/SSE conversion, aggregate address
  calculation, runtime calls, and Intel/AT&T assembly formatting.
- `src/backend/asm_optimizer.c` performs the final conservative text cleanup.
- `src/backend/native/encoder.c` encodes structured instructions directly.
- `src/backend/native/object.c` owns sections, symbols and relocations and writes
  ELF64/COFF relocatable objects. `linker.c` lays out executable images, resolves
  relocations and emits static ELF segments or PE OS import/base-relocation tables.
- `src/runtime/native_runtime.c` supplies executable startup and native runtime
  shims; system imports have private names to prevent source-symbol collisions.
  See [NATIVE_BACKEND.md](NATIVE_BACKEND.md) for image layout and limits.
- `src/diagnostics` buffers and renders text or JSON diagnostics from every phase.
- `src/driver` owns CLI validation and phase lifetime.

## AST and semantic model

Every syntax node has a source span. Expressions retain their tree shape and
evaluation order; postfix calls, indexes, and members wrap their operand rather
than reconstructing it later. Token spellings are interned once per complete
root/import graph and referenced by token index, reducing each token from a
fixed maximum-sized text buffer to a stable pointer.

Semantic symbols use stable IDs across the root program and all imported units.
Global lookup uses a hash index keyed by interned spelling and symbol kind;
pointer equality is the common comparison path while public textual lookups
remain content-correct.
Local symbols are scoped before IR lowering. Expression annotations record the
primitive type, pointer depth, named type symbol, array state, and referenced
symbol. Backend emission therefore does not decide whether source operations are
legal.

## Typed IR

IR types are primitive, named, pointer, fixed-array, or slice types. Instructions cover
constants, loads/declarations/stores, unary and binary operations, calls,
indexes, members, slice lengths, casts, allocation/free, returns, branches, jumps,
labels, and PHI values. Calls store a contiguous ordered argument slice, so
nested calls cannot corrupt argument ordering.

Boolean `&&` and `||` lower to branch/jump/label/PHI control flow and therefore
preserve short-circuit side effects. Instance methods receive an explicit hidden
aggregate pointer; implicit field names lower against that receiver. Slices expand
to data pointer and length at calls. Allocation uses a complete type and releases
are explicit. Stdlib output uses ordinary resolved overload calls.

The verifier rejects malformed type graphs, duplicate or missing value
definitions, invalid symbol ownership, nonexistent labels, ill-typed operations,
calls and returns, and bad operand or argument references before backend emission.

Every IR instruction retains its originating AST span. During x86 lowering,
that span and its IR index are attached to each structured machine instruction.
The optional `dmm-source-map-v1` artifact records this mapping with source unit
and function names, and explicitly marks generated ABI, prologue, cleanup, and
epilogue instructions as source-less. `DUMP_FORMATS.md` defines the stable
serialization contracts.

## x86-64 backend

Virtual values, parameters, and locals receive frame offsets before instructions
are emitted. Large frames are probed a page at a time. Fixed arrays and aggregate
storage live directly in the frame and are copied by value; pointers and scalar
values use eight-byte virtual slots while indirect memory operations honor their
actual element width.

System V classifies integer and SSE arguments independently and spills overflow
arguments in source order. Windows x64 uses positional registers and shadow space.
Platform I/O shims live in the standalone runtime generator. Both targets
preserve stack alignment and the RBX
callee-saved register used by address lowering. User functions that overlap the
runtime are mangled into a private DMM namespace; method symbols also encode their
owning type. Other top-level symbols remain available for C ABI interoperability.

`src/runtime/native_runtime.c` and `standalone.inc` generate machine instructions
for strings, allocation, conversions, fixed-format output, input and platform
I/O. They are embedded in assembly, objects and internal executable images.
Linux uses syscalls and static ELF startup; Windows uses kernel32 APIs and its
own file-descriptor table. The old C runtime archive has been removed.
`src/backend/runtime_calls.c` maps typed builtin calls to reserved runtime link
symbols. The emitter performs ordinary ABI argument/result lowering and contains
no syscall-number selection, Windows file-flag mapping or input implementations.
Runtime calls remain typed `IR_OP_CALL` instructions. Allocation/release and string
concatenation call compiler-owned routines; bounds checks remain backend operations.
`stdlib/io.dmm` builds higher-level I/O from typed runtime calls, while
`print`/`println` are ordinary DMM overloads. There is no automatic cleanup.

`ir_verify_control_flow` builds basic blocks with explicit and fallthrough edges,
computes entry reachability and dominators, and checks that ordinary values are
available at their uses. PHIs must begin a labeled join and name its two actual
jump predecessors; each value must be available on the corresponding edge.
Instructions cannot follow a terminator before a new label. Reachable non-void
exits require a return; void bodies retain their implicit-return convention.

## Tests

The regression corpus assembles and runs every positive program in Intel and
AT&T syntax. Separate suites cover C ABI interoperability, rejection diagnostics,
runtime bounds traps, imports and cycles, CLI behavior, architecture boundaries,
large dynamic compiler state, AST construction, IR verification, optimization,
and lexer/syntax conversion behavior. Parser, semantic, and IR libFuzzer
targets reuse the in-memory frontend API; the IR target also mutates safe,
non-owning instruction fields to exercise malformed-module verification.
