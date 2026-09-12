# Compiler roadmap

The P0 frontend and typed-IR migration is complete. The production pipeline is:

```text
source -> lexer -> structured AST -> semantic analysis -> verified typed IR
       -> x86-64 emitter -> assembly cleanup
```

There is one code-generation path. The compatibility backend and its combined
parser/type-checker/emitter have been removed from the build and source tree.

## Completed P0 work

- Full structured AST for imports, functions, structs, enums, types,
  statements, and precedence-aware expressions, with source spans.
- Separate semantic pass for symbols, scopes, named types, conversions,
  lvalues, calls, returns, member access, loop control, and constant bounds.
- Target-neutral typed IR with explicit values, calls, aggregate access,
  allocation, cleanup, labels, branches, jumps, and short-circuit PHI nodes.
- Verified IR as the sole x86-64 backend input.
- System V and Windows x64 call lowering, including mixed integer/SSE and stack
  arguments, imported functions, instance-method receivers, and mangled methods.
- Native lowering for numeric types, strings, arrays, structs, enums, pointers,
  heap allocation, `@gc`, printing, input, filesystem intrinsics, and stdlib I/O.
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
| P1 (complete) | Language syntax and stdlib ABI revision | Package imports, slices, constants, overloads, normalized arrays/pointers, explicit ownership, and ordinary stdlib output are implemented and validated. |
| P2       | Runtime library split                   | Move platform runtime shims out of the emitter while retaining typed runtime operations in IR.            |
| P2       | Direct object emission                  | Avoid the external assembler when the instruction model is mature.                                        |
| P3       | New language features                   | Add globals, richer arrays, interfaces, and generics after the middle-end remains stable. |

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
