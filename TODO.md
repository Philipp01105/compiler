# Compiler roadmap

This roadmap lists remaining work for the compiler and language. It is based on the current implementation, language
specification, runtime documentation, native-backend documentation, tests, and known limitations. Completed features are
omitted unless further work is required.

## Priorities

| Priority | Goal                                           | Exit condition                                                                                                              |
|----------|------------------------------------------------|-----------------------------------------------------------------------------------------------------------------------------|
| P1       | Define the next language edition               | Every accepted feature has specified syntax, typing, ownership, evaluation, diagnostics, and compatibility rules            |
| P1       | Make resources safe and self-hosting practical | Programs can express move-only resources, deterministic cleanup, byte buffers, and dynamic collections without relying on C |
| P2       | Complete packages, libraries, and ABI support  | Reproducible dependency resolution and stable interoperability work across supported targets                                |
| P2       | Improve compiler scalability and tooling       | Large generated programs compile predictably and IDEs can consume stable compiler services                                  |
| P2       | Maintain hosted validation                     | Linux and Windows CI cover all supported compilers, targets, optimizations, and reproducibility checks                      |
| P3       | Self-host the compiler                         | Successive compiler stages produce equivalent, deterministic artifacts                                                      |
| P3       | Add targets only after portability gates pass  | A new target has a documented ABI, object writer, linker path, runtime, diagnostics, and conformance suite                  |

## P1 execution order

1. [x] Complete language-edition rules.
    - Current and only supported edition: `2026-09-22-dev`.
    - Calendar editions and their development-state suffixes are validated.
    - No backwards-compatibility guarantee exists before the first stable release.
    - `dmm.manifest` is authoritative for project-wide experimental feature gates.
2. [ ] Implement resource safety and self-hosting foundations.
    - Move-only ownership with implicit moves and checked non-owning borrows.
    - Exclusive mutable borrows with field-sensitive disjointness.
    - Deterministic destructors plus LIFO `defer` actions.
    - Conservative non-escaping borrow analysis without lifetime parameters.
    - Typed allocation failures in high-level APIs and nullable failures in `stdlib/core`.
    - Deterministic dependency-ordered runtime package initialization.

The second P1 goal begins only after the first is complete. Other language-feature work remains outside P1 unless it is
required to implement these resource-safety foundations.

## Language definition and compatibility

- [x] Use calendar edition `2026-09-22-dev`, require an exact manifest match, and document the current absence of
  backwards-compatibility guarantees.
- [ ] Maintain a conformance index mapping each syntax form and semantic rule to parser, semantic, IR, backend, and test
  coverage.
- [ ] Specify integer overflow, shift, cast, comparison, evaluation-order, aliasing, alignment, and padding behavior
  normatively.
- [ ] Specify which implementation limits are stable language limits and which must become dynamic.
- [x] Defer deprecation and migration guarantees until the first stable release; breaking development changes may occur
  without compatibility modes or advance notice.
- [x] Gate experimental features project-wide through `dmm.manifest`; reject unknown and duplicate feature names.

## Language features

### Types, values, and memory

- [x] Support nested arrays and slices throughout parsing, type checking, constant evaluation, lowering, ABI
  classification, and code generation.
- [x] Implement and specify contextual fixed-array assignment by value; array equality remains undefined.
- [x] Add contextually typed fixed-array/slice literals, cyclic repetition, local and package backing storage, call
  temporaries, and returned hidden-backing ownership transfer.
- [x] Add subslicing and a deliberate rule for slice equality and ordering.
- [x] Add monomorphic function types and non-capturing callable values, plus compile-time polymorphic callable
  identities, contextual specialization, indirect calls, storage, equality, and null-call traps.
- [ ] Add closures only after capture lifetime and ownership rules are specified.
- [ ] Add a never type so terminating operations such as `exit` and `trap` participate correctly in control-flow typing.
- [ ] Support aggregate constants and compile-time construction of arrays, structs, enums, and tagged variants.
- [ ] Support runtime initialization of package variables with deterministic cross-package ordering and cycle
  diagnostics.
- [ ] Define nullability explicitly instead of encoding absence through unchecked pointer conventions.
- [ ] Decide whether pointer arithmetic is a supported unsafe operation; otherwise keep rejecting it with targeted
  diagnostics.
- [ ] Add volatile and atomic operations only with a documented memory model.

### Ownership and resource lifetime

Accepted P1 design: ownership is derived from concrete types. A destructor makes an ordinary `struct` move-only and
in need of destruction; both properties propagate through fields. Ordinary values remain copyable when all their
contents are copyable. Borrows are checked conservatively without general lifetime parameters. Mutable borrows are
exclusive and borrowing is field-sensitive when disjointness is provable.

- [x] Add explicit `COPYABLE`, `MOVE_ONLY`, and independent `NEEDS_DROP` type properties; derive them from normal
  struct destructors and concrete field types (including generic specializations); add implicit moves, reinitialization
  after a move, use-after-move diagnostics, and an initial ban on partial moves.
- [x] Add checked `&T` / `&mut T` types, explicit borrow expressions, explicit checked-reference-to-`*T` casts,
  field-sensitive conflict checking, conservative last-use lifetimes, and deferred-closure borrow retention.
- [x] Restrict borrowed returns to one statically provable borrowed-parameter or package-storage origin; reject checked
  references in aggregate and package-variable storage where their lifetime cannot be expressed.
- [x] Lower LIFO `defer` calls and anonymous bodies on scope fallthrough, `return`, `break`, and `continue`;
  deferred calls capture evaluated operands and deferred anonymous bodies capture locals by reference.
- [x] Lower local-value destructors and recursive owned-field/array-element destruction in reverse order; use explicit
  IR ownership effects and runtime initialization flags to suppress destruction of moved-from locals.
- [x] Extend exactly-once cleanup and moved-state flags to owned by-value parameters.
- [x] Add exactly-once cleanup for initialized package storage at normal process exit, in reverse declaration order;
  reject moves out of package storage because moved-state cannot be tracked soundly across functions.
- [x] Extend ownership state merging across all branches, loops, enum payloads, arrays, generic substitutions, and
  interface conversions; reject unsupported move-only interface erasure and diagnose invalid copies or double release.
- [x] Extend the implemented local slice-copy/backing-owner tracking through arbitrary calls, returned borrowed views,
  aggregate storage, subslices, and precise mutable regions so slices participate fully in the borrow checker.
- [x] Use explicit ownership with checked non-owning borrows as the P1 memory model; retain raw pointers as the explicit
  unchecked boundary.
- [x] Provide move-safe owning `Bytes`, `Buffer<T>`, `List<T>`, and `String` types, with explicit
  `String.view() -> string` borrowing and typed allocation failures.

### Generics, interfaces, and sum types

- [ ] Generalize concrete-to-interface conversion beyond array and slice elements.
- [ ] Permit interface values in variables, parameters, returns, fields, variants, and collections with one consistent
  representation.
- [ ] Design interface inheritance, associated types, and default methods.
- [ ] Define object-safe `Self` rules and support valid dynamic dispatch through interface elements.
- [ ] Improve generic inference, constraint diagnostics, specialization controls, and duplicate-instantiation
  elimination.
- [ ] Support recursive and mutually recursive generic types where layouts are finite.
- [ ] Add richer patterns: bindings, nested destructuring, guards, ranges, and exhaustive match expressions.
- [ ] Define stable exported layout and ABI rules for enums, variants, and interface values.

### Functions, control flow, and error handling

- [x] Add first-class non-capturing callbacks through parameters and return values.
- [ ] Standardize `Option`/`Result`-style error propagation and concise propagation syntax.
- [ ] Evaluate expression forms for `if`, blocks, and `match` without compromising definite-return analysis.
- [ ] Add labeled loop control where nested loops require it.
- [ ] Add compile-time assertions and target-conditional compilation.
- [ ] Define attributes for layout, linkage, calling convention, deprecation, diagnostics, and test discovery.
- [ ] Keep exceptions out until unwinding and cleanup behavior are fully designed.

### Source language and declarations

- [ ] Decide whether identifiers remain ASCII-only or gain normalized Unicode support.
- [ ] Add raw and multiline string literals with unambiguous escaping and source locations.
- [ ] Specify documentation comments and expose their structure to tooling.
- [ ] Decide whether partial declarations or declaration merging are needed; otherwise document their rejection.

## Standard library and runtime

- [ ] Add self-hostable byte buffers, vectors, string builders, hash maps, sets, deques, and ordered collections.
- [ ] Add iterator protocols after function values and ownership semantics are stable.
- [ ] Add UTF-8 validation, code-point iteration, searching, splitting, joining, and conversion APIs.
- [ ] Add portable path and filesystem APIs with typed errors.
- [ ] Expose command-line arguments and environment variables.
- [ ] Complete integer and floating-point parsing and formatting for every width, base, sign, precision, and error case.
- [ ] Move compatibility runtime operations such as scanning, concatenation, and floating conversion into DMM code where
  practical.
- [ ] Add allocator interfaces, arenas, bounded allocators, and allocation-failure injection.
- [ ] Link only the runtime routines reachable from the program.
- [ ] Extend I/O with seeking, file sizes, directories, terminal detection, pipes, and process execution.
- [ ] Revisit concurrency only after atomics, ownership, thread-local state, and runtime scheduling requirements are
  specified.
- [ ] Provide a DMM-native test and assertion library.

## Modules, dependencies, and builds

- [ ] Add remote dependency retrieval backed by a content-addressed cache.
- [ ] Define deterministic semantic-version selection and conflict reporting.
- [ ] Add a lockfile recording exact versions, source identity, and checksums without implicit mutation during normal
  builds.
- [ ] Add explicit offline, frozen, update, vendor, and cache-verification workflows.
- [ ] Diagnose checksum mismatch, path escape, version conflict, archive traversal, and dependency cycles precisely.
- [ ] Add workspaces or multi-package builds with one dependency graph.
- [ ] Cache parsed modules, semantic results, IR, and objects by content and configuration hash.
- [ ] Support deterministic parallel compilation.
- [ ] Put optimization profile, target, CPU features, runtime mode, library kind, and linker selection in the manifest.
- [ ] Emit a stable machine-readable build plan for external build systems and IDEs.

## ABI, libraries, linker, and targets

- [ ] Define stable unmangled C imports and exports, calling-convention attributes, header generation, and ABI
  conformance tests.
- [ ] Emit and consume static libraries.
- [ ] Teach the internal linker to consume supported external objects and archives, or provide a documented
  external-linker handoff.
- [ ] Add position-independent code and shared libraries after symbol visibility and relocation rules are specified.
- [ ] Emit source-level debug information: DWARF on ELF/COFF-compatible paths and PDB integration where applicable.
- [ ] Emit Windows unwind metadata for functions that require it.
- [ ] Add function/data sections and dead-section elimination.
- [ ] Version the compiler/runtime ABI and reject incompatible artifacts clearly.
- [ ] Add AArch64 only after target-independent IR, ABI tests, object writing, and runtime portability gates pass.
- [ ] Treat Mach-O, additional operating systems, JIT, and dynamic loading as separate proposals with complete support
  matrices.

## Compiler architecture and correctness

- [ ] Split large passes and internal `.inc` implementation fragments where that improves ownership, indexing, and
  testability without exposing private APIs.
- [ ] Improve parser recovery so independent syntax errors remain diagnosable.
- [ ] Add an incremental-analysis API supporting in-memory buffers, versioned snapshots, cancellation, and partial
  results.
- [ ] Keep name resolution, type checking, constant evaluation, ownership checking, lowering, and optimization as
  explicit passes.
- [ ] Centralize unsafe memory operations behind small audited interfaces.
- [ ] Extend IR before adding language features that otherwise require backend-specific semantic shortcuts.
- [ ] Keep the IR verifier authoritative for types, control flow, calls, aggregates, interfaces, and ownership
  invariants.
- [ ] Version serialized cache data and test rejection of stale or corrupted entries.
- [ ] Add failure-injection tests for allocation, file I/O, dependency loading, object emission, and linking.
- [ ] Remove remaining host path, locale, timezone, username, and iteration-order dependencies from generated artifacts.

## Optimization and performance

- [ ] Replace simple register assignment with a measured allocator supporting spills, coalescing, and target
  constraints.
- [ ] Add sparse conditional constant propagation, global value numbering, loop simplification, induction optimization,
  and strength reduction.
- [ ] Add costed inlining and interprocedural optimization before considering link-time optimization.
- [ ] Add alias and escape analysis sufficient to remove proven aggregate copies and stack allocations.
- [ ] Reduce unnecessary aggregate copying and oversized stack frames.
- [ ] Track compile time, peak memory, output size, startup time, and runtime benchmarks with regression budgets.
- [ ] Stress generated and adversarial programs to detect quadratic parser, semantic, monomorphization, and linker
  behavior.

## Diagnostics and developer tooling

- [ ] Implement the existing unused and deprecated warning categories and add unreachable-code, unused-import,
  unused-declaration, shadowing, lossy-conversion, ignored-result, and ownership warnings.
- [ ] Add warning groups, per-warning control, source-level suppression, and warnings-as-errors.
- [ ] Attach machine-applicable fix-its where a transformation is unambiguous.
- [ ] Add related-location traces for imports, generic instantiations, interface requirements, ownership moves, and
  dependency resolution.
- [ ] Guarantee deterministic diagnostic ordering and stable diagnostic identifiers.
- [ ] Expose tokens, syntax trees, symbols, types, references, completion, signature help, rename, and formatting
  through a compiler service consumable by JetBrains IDEs and LSP clients.
- [ ] Add a canonical formatter with edition-aware stability.
- [ ] Generate API documentation from documented declarations.
- [ ] Improve driver UX with response files, color control, verbosity levels, and documented stable exit codes.

## Validation, security, and releases

- [ ] Confirm hosted Linux CI and keep Linux and Windows builders required.
- [ ] Test strict-warning builds with supported GCC, Clang, MinGW, and MSVC configurations.
- [ ] Differentially compare unoptimized and optimized execution, assembly and native backends, internal and external
  linking, and bootstrap stages.
- [ ] Add property tests for lexer spans, parser round trips, type/layout invariants, IR verification, relocations, and
  deterministic serialization.
- [ ] Extend fuzzing to modules, manifests, generics, ownership, object writers, linkers, and machine-readable
  diagnostics.
- [ ] Test hostile manifests, dependency archives, path traversal, symlink behavior, oversized inputs, and resource
  exhaustion.
- [ ] Verify reproducible artifacts across checkout paths, locale, timezone, username, parallelism, and supported host
  compilers.
- [ ] Publish a support matrix, release checklist, artifact checksums, migration notes, and rollback procedure.
- [ ] Run long fuzz, sanitizer, performance, and bootstrap jobs separately from fast presubmit tests.

## Self-hosting

### Required language and library subset

- [x] Typed size and alignment queries.
- [x] Explicit allocation and release primitives.
- [x] Basic filesystem and stream primitives.
- [x] Deterministic native executable and object emission.
- [x] Local manifests, synchronized dependencies, and vendored dependencies.
- [ ] Move-safe byte buffers, strings, and dynamic collections.
- [ ] Stable library exports and external object/archive interoperability.
- [ ] Runtime package initialization.
- [ ] Explicit file and allocation error handling.
- [ ] Deterministic cleanup for owned resources.
- [ ] Remote dependency retrieval, checksums, and lockfiles.

### Porting sequence

- [ ] Port common data structures and diagnostics.
- [ ] Port the lexer and parser.
- [ ] Port module loading and semantic analysis.
- [ ] Port IR construction, verification, and optimization.
- [ ] Port native code generation, object writing, and linking.
- [ ] Build stage 2 with stage 1, then build stage 3 with stage 2.
- [ ] Compare stage 2 and stage 3 diagnostics, IR, objects, executables, tests, and reproducibility hashes.
- [ ] Keep the C implementation until the self-hosted replacement matches failure behavior, fuzz coverage, diagnostics,
  optimization results, and cross-target output.

## Definition of done for a roadmap item

An item is complete only when:

- [ ] Its syntax, semantics, compatibility, and ABI impact are documented.
- [ ] Parser, semantic, IR, verifier, optimizer, backend, runtime, and tooling layers are updated where applicable.
- [ ] Positive, negative, boundary, cross-module, and cross-target tests exist.
- [ ] Diagnostics identify the source cause and include related locations or fix-its where useful.
- [ ] Fuzzing or property tests cover parsers and binary formats affected by the change.
- [ ] Performance and reproducibility are measured when the item can affect them.
