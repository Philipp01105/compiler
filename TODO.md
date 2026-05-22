Compiler roadmap

The P0 frontend and typed-IR migration is complete. The production pipeline is:

```text
source -> lexer -> structured AST -> semantic analysis -> verified typed IR
       -> structured x86-64 instructions -> assembly OR native ELF/COFF objects
       -> internal ELF/PE linker for native executable output
```

There is one code-generation path. The compatibility backend and its combined
parser/type-checker/emitter have been removed from the build and source tree.

## Completed P0 work

- Full structured AST for imports, functions, structs, enums, types,
  statements, and precedence-aware expressions, with source spans.
- Separate semantic pass for symbols, scopes, named types, conversions,
  lvalues, calls, returns, member access, loop control, and constant bounds.
- Target-neutral typed IR with explicit values, calls, aggregate access,
  allocation, explicit releases, labels, branches, jumps, and short-circuit PHI nodes.
- Verified IR as the sole x86-64 backend input.
- System V and Windows x64 call lowering, including mixed integer/SSE and stack
  arguments, imported functions, instance-method receivers, and mangled methods.
- Native lowering for numeric types, strings, arrays, structs, enums, pointers,
  manual heap allocation, ordinary stdlib output calls, input, filesystem
  intrinsics, and stdlib I/O. P1 removed `@gc` and automatic cleanup.
- Nominal aggregate checking and by-value struct initialization, assignment,
  parameters, nested storage, and returns; first-class enum values and constant
  scalar payload lookup.
- Typed verifier coverage for every opcode, including operations, calls, returns,
  aggregate access, control flow, and malformed-definition mutation tests.
- Collision-safe method/runtime symbol mangling while preserving ordinary
  top-level names for C ABI interoperability.
- Dual Intel/AT&T execution tests plus independent ABI, diagnostics, import,
  runtime-failure, stress, and IR-native tests.
- Structured x86-64 instructions and typed operands with data-driven Intel and
  AT&T printers; the IR emitter no longer formats machine instructions directly.
- Module-wide interned token spellings shared across imports, plus a hashed
  semantic symbol index for fast identifier lookup.
- Dedicated libFuzzer targets and seed corpora for parser, semantic analysis,
  IR lowering, and malformed-IR verification, with a portable CTest smoke path.
- Versioned deterministic AST and typed-IR dumps plus per-x86-instruction source
  maps that retain source unit, function, IR index, and exact source span.

## Next priorities

| Priority | Addition                                | Why                                                                                                       |
|----------|-----------------------------------------|-----------------------------------------------------------------------------------------------------------|
| P1 (complete) | Artifact safety and verifier hardening | Loaded-source inventory, canonical paths/file identity, CFG dominance and PHI edge verification are implemented and covered by regressions. |
| P1 (implemented) | Standalone generated programs | Every output embeds the own machine-code runtime. Static ELF uses Linux syscalls; PE imports only kernel32. Windows 26/26 and strict GCC pass; Linux/sanitizer CI validation remains. |
| P2 (complete) | Runtime library split               | Runtime generation lives outside the emitter; IR retains typed calls. The standalone runtime supersedes the initial installed C archive. |
| P2 (implemented) | Direct object emission and internal linking | Direct x86-64 encoding, ELF/COFF objects and ELF/PE executable images are implemented; Windows execution and cross-format binary checks pass. Linux execution awaits CI. |
| P3       | New language features                   | Add globals, richer arrays, generics, traits, and richer enums/sum types after the middle-end remains stable. |
| P3       | Selfhosting | Broaden the language until its lexer, parser, semantic analysis, IR and backend can be implemented in DMM; keep the C compiler as bootstrap until staged builds agree. |

- [x] Executables are the default CLI output; `-c` selects objects and `-S`
  selects assembly. Help, examples and assembly-specific tests use explicit modes.

## P3 language foundation: generics, traits, and sum types

P3 should extend DMM without weakening its current static, nominal type model. The
preferred foundation is compile-time generics, explicit traits, and tagged sum
types. A universal dynamically typed `any` value is not required for these
features and should not be introduced as their implementation mechanism.

The design goals are:

- preserve static type checking and deterministic overload resolution;
- keep ordinary concrete values unboxed and preserve the current native ABI when
  no abstraction requires otherwise;
- make generic code reusable without adding an implicit garbage collector or
  hidden ownership model;
- allow user-defined types to participate in common operations through explicit
  trait implementations;
- evolve the existing enum feature into safe tagged unions with payloads and
  exhaustive pattern matching;
- keep all generic, trait, and sum-type information explicit in the AST, semantic
  model, typed IR, diagnostics, dumps, and mangling rules.

### Generics

Generics are compile-time type parameters on functions and named aggregate types.
The initial model should use monomorphization: every concrete generic instantiation
produces an ordinary typed function or aggregate before backend lowering. This
keeps the verified IR concrete and avoids a mandatory boxed runtime representation.

Target syntax:

```dmm
func identity<T>(value:T) -> T {
    return value;
}

func first<T>(values:T[]) -> T {
    return values[0];
}

struct Pair<A, B> {
    var first:A;
    var second:B;
}
```

Generic type parameters are nominal compile-time symbols and may appear anywhere a
normal type is accepted, subject to the same sized/unsized restrictions as other
types. A generic declaration is checked once structurally, then each concrete
instantiation is checked again after substitution for operations that depend on
capabilities or layout.

Initial generic rules:

- Type arguments are inferred from call arguments when there is exactly one valid
  substitution; explicit type arguments may be added later if inference is not
  sufficient.
- Generic parameters are invariant. `Container<Derived>` is not implicitly
  convertible to `Container<Base>`.
- Pointer depth, array length, slice element type, and nominal aggregate identity
  remain part of the instantiated type.
- Generic functions participate in overload resolution only after a valid type
  substitution has been found. A concrete non-generic exact match should win over
  an otherwise-equivalent generic candidate.
- Return type alone never determines generic inference, matching the existing rule
  that return type does not distinguish overloads.
- `void` is not a valid type argument where a sized value is required.
- Recursive instantiation must be cycle-checked and the compiler must impose a
  deterministic instantiation-depth/total-instantiation limit.

Example:

```dmm
func max<T: Ordered>(a:T, b:T) -> T {
    if (a < b) { return b; }
    return a;
}
```

The `T: Ordered` form is a trait bound. Unconstrained generic code must not assume
operators, methods, formatting, copying behavior beyond what is valid for every
possible substituted type.

### Traits

Traits define compile-time capabilities that named types may implement. They are
not classes and do not introduce inheritance. The first implementation should use
static dispatch for generic trait bounds; runtime trait objects can be considered
later as a separate feature.

Target syntax:

```dmm
trait Printable {
    func toString() -> string;
}

trait Equal {
    func equals(other:*Self) -> bit;
}

impl Printable for Person {
    func toString() -> string {
        return name;
    }
}
```

`Self` refers to the implementing nominal type. An `impl` is valid only when all
required members are present with exactly compatible signatures. Duplicate or
conflicting implementations are compile errors.

Initial trait rules:

- Trait satisfaction is explicit; structural method-name matching alone does not
  implement a trait.
- A type may implement multiple traits.
- A trait may be used as a generic bound, for example `T: Printable`.
- Multiple bounds should use a deterministic syntax such as
  `T: Printable + Equal`.
- Trait methods used through a generic bound are resolved during semantic analysis
  and monomorphized to ordinary concrete calls.
- Trait implementations follow module visibility and nominal type identity. Two
  same-named structs from different modules are distinct implementers.
- The first trait revision should not include trait inheritance, default method
  bodies, associated types, higher-kinded types, or runtime `dyn Trait` values.
  These can be added independently after the core model is stable.

Traits provide the long-term replacement for using an unrestricted `any` type for
operations such as printing. The standard library can retain primitive overloads
while also exposing generic helpers constrained by `Printable` when user-defined
implementations become available.

For example:

```dmm
func printValue<T: Printable>(value:T) -> void {
    print(value.toString());
}
```

This keeps unsupported values as compile-time errors rather than deferring type
checks to runtime.

### Enums as tagged sum types

DMM already has first-class enums with named variants and constant scalar payload
fields. P3 should evolve this into general tagged sum types where each variant may
carry its own typed payload. The runtime representation is a discriminant/tag plus
storage large and aligned enough for the largest variant payload.

Target syntax:

```dmm
enum Result<T, E> {
    Ok(T),
    Err(E),
}

enum Option<T> {
    Some(T),
    None,
}

enum Token {
    Integer(int),
    Identifier(string),
    End,
}
```

Unlike the current enum field model, payload shape belongs to the individual
variant. Values therefore require checked destructuring before variant payloads
may be read.

Pattern matching should be introduced together with payload-bearing variants:

```dmm
func unwrapOr<T>(value:Option<T>, fallback:T) -> T {
    match (value) {
        Some(v) => return v;
        None => return fallback;
    }
}
```

Initial sum-type rules:

- Every enum value stores exactly one active variant.
- Access to variant payloads is legal only after a successful pattern match or an
  equivalent compiler-proven tag check.
- `match` on an enum must be exhaustive unless an explicit wildcard arm is present.
- Duplicate or unreachable variant arms are diagnostics.
- Generic enum payloads are substituted before layout is computed.
- Equality is not implicit for every enum. It is available only when the language
  defines it for all payloads or when a suitable trait bound/implementation exists.
- By-value enum parameters, assignments, and returns keep the current aggregate
  semantics; large-value ABI lowering may use the same target-specific aggregate
  rules already used for structs.
- Payload ownership remains explicit. A variant containing a pointer or owned
  string does not gain automatic destruction merely because it is stored in an
  enum.

This form supports common safe APIs without introducing null or untyped dynamic
values:

```dmm
func find(values:int[], needle:int) -> Option<int>;
func parseInt(text:string) -> Result<int, ParseError>;
```

### Interaction with overloads and `print`

Existing overloads remain useful and are not replaced by generics or traits.
Overloads select between different concrete APIs; generics reuse one implementation
for multiple types; traits constrain which types a generic implementation may use.

The intended progression for output is therefore:

```text
P1/P2: print(int), print(float), print(string), ...
              |
              v
P3:     generic helpers constrained by Printable
              |
              v
later:  optional formatting traits / formatter objects
```

The language should not define `print(value:any)` as the primary abstraction.
Doing so would require runtime type metadata, boxing/tagging rules, dynamic failure
semantics, and ownership rules that are otherwise unnecessary for DMM's static
model.

### Semantic and IR model

The frontend and middle-end should represent these features explicitly rather than
hiding them in parser sugar.

- AST: add generic parameter lists, trait declarations, implementation blocks,
  generic type arguments, variant-specific enum payload declarations, `match`,
  patterns, and bound syntax.
- Semantic analysis: add type-variable symbols, substitution/unification,
  instantiation caches, trait tables, coherence/conflict checks, bound checking,
  exhaustiveness analysis, and variant-flow refinement.
- Typed IR: lower only fully instantiated concrete types and functions in the
  initial design. Sum types lower to an explicit tag plus payload storage and
  `match` lowers to verified tag branches plus payload extraction.
- Verifier: reject unresolved generic parameters, invalid trait dispatch,
  out-of-range variant tags, payload extraction from the wrong variant, malformed
  match control flow, and layout/type mismatches.
- Backend: receives concrete layouts and calls after monomorphization, so generic
  dispatch itself requires no new machine-level ABI. Sum-type layout and aggregate
  passing must be implemented for both System V and Windows x64.

### Generic symbol identity and mangling

Instantiated generic declarations require deterministic type-derived identities.
The mangled symbol must include the generic declaration identity and every concrete
type argument in source order using the same canonical module-qualified type
encoding already used for overloads.

Conceptually:

```text
identity<int>          -> generic identity + <int>
Pair<int,string>       -> nominal Pair + <int,string>
max<MyType>            -> generic max + <module::MyType>
```

Two identical instantiations in one program must canonicalize to one generated
specialization. Instantiation order must not affect emitted names or binary output.

### Implementation sequence

Generics, traits, and sum types should be staged so each step leaves the compiler
in a testable state:

1. **Generic type infrastructure:** type variables, generic parameter AST nodes,
   canonical substitutions, generic identity in dumps, and monomorphization cache.
2. **Generic functions:** inference from arguments, specialization, recursion
   protection, overload interaction, mangling, and positive/rejection tests.
3. **Generic structs/enums:** instantiated aggregate identity, layout, member access,
   assignment, calls, returns, and ABI coverage.
4. **Traits and `impl`:** trait symbols, implementation tables, `Self`, bound
   validation, static dispatch, conflict diagnostics, and generic constrained calls.
5. **Variant-specific enum payloads:** tagged layout, construction, storage,
   parameter/return support, and verifier coverage.
6. **Pattern matching:** pattern AST, exhaustiveness/reachability checks, payload
   binding, control-flow lowering, diagnostics, and source maps.
7. **Standard library adoption:** introduce reusable generic containers and
   `Option<T>`/`Result<T,E>`; add traits such as `Printable`, `Equal`, and later
   ordering/formatting capabilities only where concrete use cases justify them.
8. **Acceptance:** cross-platform execution, deterministic AST/IR dumps and native
   output, sanitizer/fuzz coverage, overload/generic ambiguity tests, recursive
   instantiation limits, trait-conflict tests, exhaustive-match tests, and ABI tests
   for small/large instantiated sum types.

The first P3 milestone is complete when generic functions and generic aggregate
values can be fully type-checked and monomorphized without backend knowledge of
open type variables. The second milestone is complete when trait-bounded generic
code and payload-bearing enums with exhaustive matching execute through the same
verified IR and native backends as ordinary concrete DMM code.

## Standalone runtime migration and selfhosting

The immediate goal is independence of generated programs from libc, Windows CRT,
third-party libraries and foreign language runtimes. The compiler remains in C
for now. OS interaction is unavoidable and explicit: Linux syscalls and Windows
system APIs. This requirement applies to assembly, native objects and internally
linked executables. Implementation is complete; Linux and sanitizer execution
remain explicit CI acceptance checks.

- [x] All output modes: string length, comparison, copy and append emit their own
  integer instructions; duplication uses the new allocator.
- [x] All output modes: allocation/free use Linux mmap/munmap or Windows
  VirtualAlloc/VirtualFree, with a 16-byte allocation header/alignment and fresh
  zeroed mappings for calloc. This initial allocator maps each allocation separately; a reusable
  arena/pool allocator is a later performance improvement.
- [x] Replace runtime I/O and descriptors, input parsing, integer/float formatting
  and exit/startup. Eliminate msvcrt.dll, libc.so.6 and the ELF interpreter.
- [x] Embed the same own runtime in assembly and object output and remove the
  generated-program requirement for the C dmm_runtime archive.
- [x] Assert that ELF executables have no dynamic dependencies and PE executables
  import only OS DLLs. Reject unresolved libc/CRT symbols rather than falling back.
- [x] Windows: 26/26 CTest suites, including assembly/object links with -nostdlib,
  kernel32-only dependency checks, native runtime execution and 2,000 binary64
  formatting comparisons; strict GCC compiler build passes.
- [ ] Confirm Linux, sanitizer and Clang fuzz CI through the standalone paths.
  Structural ELF tests forbid PT_INTERP/PT_DYNAMIC; local Linux execution is unavailable.
- [ ] Add selfhosting building blocks: fixed-width and pointer-size integers,
  byte buffers/slices, dynamic collections, module/library compilation without
  a mandatory main, explicit file errors and deterministic artifact writing.
- [ ] Port compiler stages to DMM incrementally; bootstrap stage 1 with C, compile
  stage 2 with DMM, and compare deterministic compiler outputs and regressions.

## Reanalysis checkpoint (2026-09-13)

Reviewed the current working tree, including the uncommitted diagnostics and IDE
changes. `cmake --build compiler/cmake-build-debug` reported no work to do;
`ctest --test-dir compiler/cmake-build-debug --output-on-failure` passed **22/22**
suites on Windows/MinGW. The existing build has `DMM_STRICT_WARNINGS=OFF`.
This checkpoint does not revalidate Linux, sanitizers, a fresh strict-warning
build, or the JetBrains plugin. The earlier 19-suite cross-platform acceptance
below remains a historical checkpoint, not the current suite count.

The findings below originated in source review. Follow-up implementation passed
23/23 Windows CTest suites and a separate fresh strict-warning compiler/runtime build in
`cmake-build-review` (`BUILD_TESTING=OFF`). Source-overwrite regressions use
isolated temporary fixtures; cleanup flush/close/replace failures are injected
only in the optimizer test target. Windows hardlink and case checks pass;
directory-symlink checks are skipped because the OS does not grant symlink
creation rights. Linux and sanitizers were not rerun.

- [x] **P1 — Protect every loaded source from every generated artifact.**
  `src/driver/main.c` validates requested artifacts against the root and a separate
  inventory of opened imports, including imports rejected by the lexer, before
  output removal or opening. This includes the IDE AST dump. The
  implicit `.s` output is checked before deletion and emission. If the frontend
  returns no program, stale assembly is retained because source-safe deletion
  cannot be established.
  Validation: `cli_contract` checks assembly/AST/IR/source-map collisions with
  root, direct and transitive imports, IDE dump collisions, and an imported source
  at the implicit `.s` path; hashes remain unchanged and JSON reports `C102`.
  Rejection tests verify lexical failures preserve stale output without writing
  new assembly; parser and semantic failures still clear safe stale output.
- [x] **P1 — Detect artifact aliases using canonical paths and file identity.**
  `src/common/path_identity.c` handles canonical existing paths, missing-output
  parents, Unix inode/device identity and Windows volume/file identity. Both
  sources and artifact pairs use the same checks before destructive output
  actions. Unresolvable identities and dangling links fail closed.
  Validation: `cli_contract` covers `./`/`..`, missing output aliases, Windows
  case variants and hardlinked sources. Directory-symlink alias tests are present
  but require an OS that permits symlink creation.
- [x] **P1 — Verify CFG dominance and PHI predecessor edges.**
  `src/ir/ir_cfg.c` builds blocks, explicit/fallthrough edges, predecessor lists,
  reachability and dominators. Ordinary value definitions must dominate reachable
  uses; PHIs begin labeled joins, match the two actual jump predecessors and check
  value availability on each edge. Terminators cannot have trailing instructions
  before another label, and reachable non-void exits require a return.
  Lowering avoids redundant jumps after returns/breaks and records the actual
  finishing block of nested short-circuit right operands for PHI edges.
  Validation: `frontend_pipeline_unit` mutates branch-local uses, unrelated PHI
  labels, missing edges, missing returns, post-terminator instructions and PHI
  placement. Nested short-circuit expressions execute in Intel/AT&T; existing
  loop, fuzz and malformed-IR tests pass.
- [x] **P2 — Separate runtime generation from the emitter.**
  Typed calls pass through `runtime_calls.c`; ordinary ABI lowering remains in the
  emitter. The standalone machine-code runtime in `src/runtime` supersedes the
  initial C runtime archive and embeds in all outputs. Architecture checks reject
  the removed libc-backed files. Runtime tests execute the emitted instructions
  directly, including strings, allocation, parsing, numeric formatting and file I/O.
- [x] **P2 — Direct native objects and internal ELF/PE executable linking.**
  `--emit=asm|obj|exe` shares verified IR and structured instruction lowering.
  Native encoding supports the compiler's integer/SSE instructions and indexed/
  RIP-relative addressing. ELF64/COFF writers own symbols and relocations;
  the internal image linker adds standalone startup/runtime, static ELF load
  segments and PE OS import/ASLR relocation tables. It invokes no
  assembler, C compiler or platform linker. Native runtime definitions remain
  outside the emitter and private import names prevent source-name collisions.
  Native source maps record text-section byte offsets. All modes retain loaded
  source/file-alias protection. See `NATIVE_BACKEND.md` for usage and limits.
  Validation: Windows corpus execution for direct objects and internal PE images,
  C ABI interoperability, input/EOF/truncation, cross ELF/COFF emission, deterministic
  syntax-independent bytes, binary relocation/import/layout checks and native
  CLI artifact protection. All 26 Windows CTest suites and a strict warning build
  pass. Linux execution and
  sanitizer execution were not run locally; existing CI includes the new suites.
- [ ] **P2 — Confirm native ELF execution on Linux CI.**
  First CI run: Linux and sanitizer suites both failed in `pointer_semantics`
  for assembly/direct objects because source `write` intercepted libc `write`
  from the static C runtime. Internal ELF executable execution passed this case.
  Fixed shared symbol mangling for `write`, `open`, `close`, startup/errno and
  CRT aliases; expanded runtime collision execution and cross-target symbol tests.
  Also fixed Clang's misleading-indentation error in native import lookup that
  blocked the fuzz build. Windows 26/26 and strict GCC build pass after the fix;
  Linux, sanitizer and Clang fuzz CI need rerunning to confirm the changes.
  Run the new native corpus, C ABI and runtime/EOF suites on x86-64 Linux.
  Local cross-emission and structural ELF checks pass; they do not establish
  Linux syscall/runtime execution. Keep this verification explicit until CI passes.
- [x] **P2 — Always close cleanup output and preserve the actual I/O failure.**
  `cleanup_assembly_file_detailed` always attempts flush and close separately,
  records the first failure before cleanup, and reports its operation, path and
  reason through `backend.c` as `G103`. Windows replacement failures retain the
  native error code and system message. The original cleanup API remains available.
  Validation: `optimizer_unit` injects flush, close, replacement and combined
  flush/close failures, verifies output close executes once, checks removal of
  failed temporary output and preservation of the original assembly, and exercises
  the actual backend human/JSON diagnostic path for all three failure operations.
- [ ] **P2 — Finish public documentation migration.**
  `README.md` still advertises garbage-collected allocations and imports through
  `# import`, although both were removed. Audit the README and language examples
  against P1, document ordinary stdlib output and explicit allocation ownership,
  and clarify whether the language specification's integer-overflow/division
  errors apply only to constant expressions or also to runtime arithmetic.
  Acceptance: documented examples compile with the current frontend and no
  supported-feature list presents removed syntax as available.

## Diagnostic improvements

- [x] Pass unsaved root documents via `--ide-buffer` without modifying source files
  or changing import resolution. Make insertion-point diagnostics visible in the
  JetBrains editor, annotate malformed tokens immediately, reuse identical analysis
  results and terminate superseded compiler checks for the same document.
- [x] Audit every defined diagnostic constant and reporting site; add checks for
  human/JSON consistency across all rejection fixtures. Record active/unused codes
  and verification limits in `DIAGNOSTICS_AUDIT.md`. Separate type and semantic
  codes, improve expression locations, declaration/delimiter notes, conversion
  details, filesystem paths/reasons, Unicode encoding and error display limits.
- [ ] Include the failing IR instruction and its source span for internal lowering
  and backend failures; retain precise filesystem errors for output failures.
- [ ] Extend remaining operator/operand `T102` messages with supplied types.
- [ ] Add deterministic allocation and I/O fault injection to verify resource-error
  branches without depending on actual out-of-memory or disk-full conditions.
- [x] Point missing-semicolon errors at the preceding statement's end, including
  blank lines before the next token, and expose an exact insertion suggestion.
- [x] Provide `--ide --dump-ast` analysis with syntax recovery and semantic data
  for valid subtrees despite errors. Preserve hover types in the JetBrains plugin,
  discard malformed statements, and leave assembly output untouched. Cover
  multiple errors, missing closing braces, and subsequent valid declarations.
- [x] Reject unsupported method references during semantic analysis, before IR
  emission. Reproducer: a struct `Node` defines `func Next() -> *Node`, and
  `var node3 = node.Next;` previously reached the backend and failed with the
  generic `error[G103]: Could not emit typed IR output '…'`. Now report `S111`
  diagnostic at `node.Next` explaining that methods must be called with `()`;
  `var node3 = node.Next();` compiles successfully. Add a rejection regression
  test and retain a positive test for the actual method call.
- [x] Source-related errors show the filename, line and column, the
  offending source line, and a caret/underline over the relevant source span.
  Preserve this context for imported files and expose it in `--formatError`
  JSON so IDE plugins can display the same location and source context. For
  errors without a source location, explain the failing operation instead of
  inventing a line number. Cover tabs, Unicode, CRLF, and end-of-file errors.
- [x] Offer a concrete correction when it can be determined safely. For an
  unambiguous zero-argument method used as a value, suggest replacing
  `node.Next` with `node.Next()`. Only present an actionable fix when symbol
  resolution, argument requirements, and the replacement are certain; do not
  guess arguments or apply edits automatically. Otherwise give an explanatory
  hint. Include suggestions in human-readable and JSON diagnostics; structured
  fixes should carry the source file, exact replacement span, and replacement
  text so IDEs can offer a user-invoked quick fix. Test both safe suggestions
  and cases where ambiguity or required arguments must suppress a fix.

## P1 syntax migration plan

This is an intentionally breaking language revision. The lexer, parser, AST,
semantic model, IR, backend, standard library, examples, tests, fuzz corpora,
and language documents move together. The removed spellings must produce clear
diagnostics; they must not remain as aliases or activate a compatibility path.

### Target syntax

```dmm
import "math.dmm"
import (
    "collections/list.dmm"
    "stdio/io.dmm"
    "stdlib" // resolves only stdlib/package.dmm
)

var values:int[16];
var links:**Node[8];
var pointertolinks:*(**Node[8]) = &links;
const rowCount:int = 16;
func sum(values:int[]) -> int {
    var total:int = 0;
    for (var i:int = 0; i < values.length; i++) { total += values[i]; }
    return total;
}
func follow(root:***int) -> int { return ***root; }

var owned:*int = reserve(int);
free(owned);
print(value);
println("done");

*pointer = 1;
**pointer_to_pointer = 2;
(*node_pointer).value = 3;
```

- A single import is `import path`; a grouped import is `import ( path ... )`.
  Paths retain the existing quoted relative and angle-library forms. Import
  aliases and dot imports are out of scope for this revision. A path resolving
  to a file imports exactly that file. A path resolving to a directory imports
  exactly `<directory>/package.dmm`; absence of that entry file is an import
  error. The compiler never scans a directory recursively for `.dmm` files.
- `package.dmm` is ordinary DMM source and explicitly imports the package's
  public modules. For example, `stdlib/package.dmm` may import `io.dmm` and
  `string.dmm`, while internal, platform-specific, optional, and nested-package
  files remain unloaded unless the manifest explicitly references them. The
  name `stdlib` has no compiler magic. File and package entry imports share
  canonical-path deduplication and cycle detection.

  ```text
  stdlib/
    package.dmm   # imports "io.dmm" and "string.dmm"
    io.dmm
    string.dmm
    internal/     # not imported unless package.dmm selects it
  ```

- Every loaded unit receives a canonical logical module identity. It uses `/`
  separators, removes `.` segments, resolves `..` without escaping the allowed
  package/source root, and never contains a host-specific absolute directory.
  The root unit uses the fixed identity `.`; imported identities are normalized
  relative to that root or their package root. The same canonical file must
  receive the same identity regardless of the import spelling used to reach it.
- Local and struct-field arrays use `name:type[size]`. Array parameters use
  `name:type[]`; a size in a parameter declaration is rejected. Array return
  types and inferred array sizes remain unsupported.
- Pointer stars precede the base type and may have arbitrary depth everywhere a
  type is accepted: `*T`, `**T`, `***T`, and so on. Array suffixes apply after
  the complete element type, so `**T[4]` is an array of four `**T` elements.
  Parentheses group types: `*(**T[4])` is a pointer to an array of four `**T`
  elements, while `***T[4]` is an array whose elements are `***T`.
- `@gc` and automatic function-exit cleanup are removed. `reserve` allocations
  use manual ownership and the compiler must not insert hidden releases. The
  programmer is responsible for releasing each live allocation exactly once and
  for avoiding use-after-free. Existing owned-string rules remain explicit.
  This revision does not claim complete static ownership or alias analysis.
- `print(value)` and `println(value)` become ordinary functions supplied by the
  standard library, not keywords, special statements, compiler-known function
  names, receiver methods, or dedicated IR operations. The stdlib provides
  typed overloads for the supported primitive and string types and implements
  them using lower-level conversion and output facilities. Both functions
  return `void`; an empty line is `println("")`. Programs import the defining
  stdlib module normally; the compiler does not inject an implicit print prelude.
- Function overload sets and overload resolution are general language
  facilities rather than a `print` exception. Candidate selection uses arity
  and parameter types, prefers exact matches over existing implicit widening,
  diagnoses ambiguous/no-match calls, and gives overloads unique type-derived
  link names. A non-overloaded top-level function keeps its existing C ABI name.
- Dereference is exclusively a prefix unary operator. Assignment accepts a
  semantically writable expression, including `*p`, `**pp`, `p[index]`, member
  access, and parenthesized combinations. Postfix dereference such as `p* = x`
  is rejected.

### Slice parameter contract

- `T[]` is a slice type and is initially valid only as a function parameter.
  It is not an alternate spelling for `*T`: a slice carries both a data pointer
  and an element count. `T[N]`, `T[]`, and `*T` remain distinct semantic and IR
  types.
- Passing a fixed array `T[N]` to `T[]` creates the pair `{ &array[0], N }`.
  Passing a slice parameter onward copies both fields. No allocation or element
  copy occurs, and the callee may mutate elements of the original array.
- `slice.length` exposes the passed count as a read-only `int`; `slice[index]`
  uses that count for runtime bounds checks. The slice itself cannot escape by
  return, storage in a struct, or assignment to a longer-lived binding in this
  revision.
- At the native ABI boundary a slice expands, in source-parameter position, to
  two consecutive arguments: data pointer first, nonnegative DMM `int` length
  second. Register classification and stack spill advance for
  both physical arguments on System V and Windows x64. The hidden length is not
  counted as a source argument for arity or overload resolution.
- Only an array/slice with exactly the same nominal element type and pointer
  depth converts to a slice. There is no implicit `T[]`/`*T` conversion and no
  covariance between named element types.

### Function overload rules

- Top-level functions may share a name, and methods may share an owner and name,
  only when their ordered source-parameter type sequences differ. Return type,
  parameter names, `const` values, and the hidden slice length do not distinguish
  overloads. Repeating an identical signature is a duplicate declaration.
- Lookup first builds the visible overload set using the ordinary scope/import
  rules, then removes candidates with the wrong arity or an impossible implicit
  conversion. Imported declarations participate exactly like local declarations;
  two modules providing the same visible signature are an error.
- Each argument conversion has a rank: `0` for an identical type; `1` for
  `bit`/`byte`/`char` promotion to `int`, `float` promotion to `double`, or
  fixed-array-to-matching-slice conversion; `2` for another currently legal
  implicit numeric conversion. Explicit-only casts, different named types,
  unequal pointer depth, and slice/pointer conversions are not candidates.
- Candidate A is better than B only if every argument rank in A is no worse and
  at least one is better. Exactly one non-dominated candidate must remain.
  Otherwise the call is ambiguous. Declaration order, import order, and return
  context never break ties. Diagnostics list the supplied types and all viable
  signatures in mangling-independent source form.
- Method overload resolution applies the same rules to explicit arguments after
  resolving the receiver's exact nominal owner. Static and instance methods are
  separate overload sets. `main` cannot be overloaded.

### Allocation contract

- `reserve` has the grammar `reserve(type)`; its operand is parsed as a type AST,
  never as an expression. Runtime values, variables, calls, and computed element
  counts are rejected even if they evaluate to an integer.
- The operand must be a complete sized non-`void` type: primitive, pointer,
  struct, enum, or fixed array. Inferred and slice types are rejected.
  `reserve(T)` returns `*T`, `reserve(*T)` returns `**T`, and
  `reserve(T[N])` returns `*(T[N])`.
- One object of exactly that type is zero-initialized. Dynamic-count allocation
  is deliberately absent; it requires a future allocator/container API rather
  than overloading `reserve`. The programmer is responsible for eventually
  releasing the allocation exactly once through `free` using its original base
  pointer. Sema may reject locally obvious double-free, use-after-free, invalid
  free, or direct leak patterns, but aliases and control flow make this a best-
  effort diagnostic rather than a sound ownership guarantee.

### Link-name mangling

- Source overload selection is completed before mangling. The return type is not
  encoded. `main` always links as `main`; a non-overloaded top-level function
  retains its plain name for C ABI interoperability. Every function in an
  overload set is mangled, so adding an overload intentionally changes that
  set's external ABI. Methods are always mangled.
- Overloaded functions use `__dmm_f<name-bytes>_<name>__<arity>` followed by one
  `_<type>` segment per source parameter. Instance and static methods use
  `__dmm_m<owner-type>_<i|s>_<name-bytes>_<name>__<arity>` followed by the same
  parameter segments; `<owner-type>` is the full nominal encoding below rather
  than only the visible owner name.
  Lengths are decimal UTF-8 byte counts; current identifiers are ASCII, but the
  byte rule keeps the format defined for future Unicode identifiers.
- Primitive type codes are `i` (int), `c` (char), `y` (byte), `b` (bit), `f`
  (float), `d` (double), `s` (string), and `v` (void). Recursive constructors are
  `p<type>` for each pointer level, `a<N>_<type>` for a fixed array, `l<type>` for
  a slice, and
  `n<module-bytes>_<module-hex>_<name-bytes>_<name>` for a nominal type.
  `<module-hex>` is the lowercase hexadecimal UTF-8 encoding of the canonical
  logical module identity and `<module-bytes>` is its pre-encoding byte count.
  Consequently, equally named types from different modules always have distinct
  type encodings. Parentheses do not affect encoding. For example, `print(int)` becomes
  `__dmm_f5_print__1_i`, while `consume(**Node[])` ends in
  `__1_lppn<module-bytes>_<module-hex>_4_Node`.
- The slice's physical length argument is implied by `l` and is not separately
  encoded. Generated names are checked for collisions before emission, and all
  source identifiers beginning with `__dmm_` remain reserved. A non-overloaded
  top-level name that collides with a platform/runtime link symbol uses the same
  `__dmm_f...` encoding even though its overload set has one member.

### Constants

- `const name[:type] = constant-expression;` is accepted at top level and in a
  block. An initializer is mandatory; omitting the type infers it from the
  folded expression. Parameters, fields, and enum payload declarations do not
  use `const` in this revision.
- Constant expressions may contain primitive/string literals, earlier visible
  constants, parentheses, pure unary/binary operators, and explicit primitive
  casts. They may not contain calls, allocation/free, mutable variables,
  assignment, indexing, or address/dereference operations. Initial support is
  limited to primitive and string values, not structs, arrays, slices, or
  pointers.
- Constants are evaluated and type-checked during semantic analysis with the
  same overflow/division rules as ordinary expressions. Cycles, forward local
  references, nonconstant initializers, and invalid explicit target types are
  diagnostics. Positive `int` constants are valid fixed-array lengths.
- A constant is a symbol but not an lvalue: assignment, increment/decrement, and
  address-of are rejected. Lowering materializes its folded value directly as an
  IR constant and allocates no mutable stack/global storage. Top-level constants
  follow import visibility and duplicate-name rules.

### Implementation sequence

#### Completed acceptance checkpoint (2026-09-12)

The core syntax, overload selection/mangling, slice ABI, type-only allocation,
manual allocation ownership, and ordinary stdlib print path are implemented.
Repository acceptance migration now covers examples, generated regression
programs, frontend/import fixtures, removed-syntax diagnostics, and output
expectations for ordinary call evaluation. Package integration tests exercise
grouped imports, manifest-only loading, canonical deduplication, and missing
manifests. P1 execution tests cover stdlib output, overloads, deep allocation
pointers, and slice forwarding/mutation across register and stack arguments.

Constant initializers now fold during sema and materialize directly in IR;
evaluated int constants supply fixed-array lengths. Casts now use the postfix
`expression.(type)` syntax, with old type-first casts rejected. Regression
coverage includes folding, overflow/division errors, cycles/forward references,
and chained casts including byte narrowing.

P1 is complete. Constant evaluation follows the import dependency graph, including
shared dependencies; overload diagnostics list supplied types and candidate source
signatures. Grouped pointer-to-array types preserve element pointer depth, outer
pointer depth, and length through calls, assignments, returns, address-of, allocation,
and member/index access. Nested arrays and slices remain unsupported and produce
an explicit diagnostic. Slice lengths are read-only. Mangling preserves long names
and validates link-name uniqueness before emission.
The formal grammar now describes P1 and postfix casts; AST/IR dumps use v2,
with AST folded values and cast/allocation type operands serialized explicitly.

Acceptance: strict-warning builds pass on Windows/GCC and ELF/System V GCC.
All 19 CTest suites pass on both platforms, including Intel/AT&T execution,
ABI, rejection/runtime failure, imports, dump/source-map, architecture, malformed
type-graph verification, and fuzz smoke. The ELF compiler and generated programs
also pass ASan/UBSan with leak detection enabled. All 40 example code-generation
cases pass across ELF/COFF and Intel/AT&T; COFF examples also assemble and link.
Examples and heap fixtures use explicit releases, including temporary output strings.

1. **Grammar and lexer:** add `const`; remove `#import`, `@gc`, and statement-form
   `print`/`println`; add single/grouped import parsing, postfix array type
   suffixes, unsized slice-parameter suffixes, grouped types, repeated prefix
   pointer markers, and type-only `reserve`. Reserve `import` and `const`, but
   lex `print` and `println` as ordinary function identifiers.
2. **AST and parser:** represent one import declaration with an ordered path
   list, resolve directory imports exclusively through `package.dmm`, unify grouped
   declaration/field/parameter type parsing, represent slices and constants,
   make `reserve` own a type node instead of expression arguments, remove the
   separate parameter-array flag and GC bit, and parse assignment targets from
   the normal prefix/postfix expression grammar before checking lvalue validity.
3. **Semantic analysis:** validate sized versus unsized array contexts, propagate
   arbitrary pointer depths without folding arrays into pointer depth, lower
   parameter arrays to pointer-plus-length slices, fold and validate constants,
   validate type-only allocations, represent same-name functions as overload
   sets, resolve calls by typed conversion cost, enforce explicit ownership, and
   issue focused diagnostics for every removed legacy form.
4. **IR and verification:** remove `IR_OP_PRINT`, retain ordinary typed calls and
   `IR_OP_FREE`, make `IR_OP_ALLOC` reference a complete type rather than a
   runtime operand, remove GC flags and automatic-cleanup edges, add explicit
   slice data/length representation, materialize folded constants, encode the
   selected overload symbol in each call, and verify all new type shapes.
5. **Standard library and runtime boundary:** implement `print`/`println`
   overloads for `int`, `char`, `byte`, `bit`, `float`, `double`, and `string` in
   the stdlib. Build them from neutral low-level byte-output and value-to-string
   facilities; these primitives may remain runtime intrinsics, but neither their
   names nor IR semantics may be `print`-specific.
6. **x86-64 backend:** delete print-opcode lowering, GC-local discovery, and
   epilogue cleanup emission; implement the specified type-derived symbols and
   canonical module-qualified nominal encodings plus the two-argument slice ABI;
   audit load/store/address lowering for more than one
   pointer level, grouped pointer-to-array types, and arrays whose elements are
   pointers; preserve System V and Windows parameter ABI behavior.
7. **Repository migration:** mechanically convert the standard library,
   examples, positive/rejection/runtime tests, ABI fixtures, fuzz seeds, and
   documentation. Update `dmm-ast-v1`/`dmm-ir-v1` only through new format
   versions if their serialized schema must change.
8. **Acceptance:** all converted programs compile without a legacy parser;
   dedicated rejection tests cover every old spelling plus ambiguous/no-match
   overloads; package-manifest tests prove that unlisted/internal files are not
   loaded and missing manifests are rejected; deep-pointer, slice-length/bounds,
   grouped-type, type-only-reserve,
   constant-folding, grouped-import, stdlib-print, mangling, and explicit-free
   programs execute in Intel/AT&T syntax on ELF/System V and COFF/Windows;
   sanitizer, fuzz-smoke, import, dump/source-map, architecture, and full
   regression suites pass.