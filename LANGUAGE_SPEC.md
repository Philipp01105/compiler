# DMM Language Specification 0.3

Status: experimental. This document defines the tested source-language contract; undocumented behavior may change.

## Source and declarations

A source file is UTF-8 text beginning with `package name;`, followed by imports, structs, enums, traits,
implementations, constants, package variables and functions. Statements and imports end with `;`. `//` introduces a line
comment; `/* ... */` introduces a non-nesting block comment that may span lines. Unclosed block comments are lexical
errors. Execution begins in a parameterless `main` returning `void` or `int` in `package main`; library packages need no
entry point.

Each project has one `dmm.manifest` defining its module path and required language edition. One directory is one
package; all `.dmm` files directly in it are discovered and share a declaration scope. Package identity is the module
path plus relative directory. All files agree on their package name. `package.dmm` has no special meaning.

`dmm manifest sync` reconciles requirements across the whole module, marks dependencies used only transitively with
`// indirect`, and removes unused entries. It uses exact versions from local manifests; ordinary builds never rewrite
the project manifest.

Imports use quoted canonical package paths, for example `import "github.com/example/project/lexer";` or
`import lex "github.com/example/project/lexer";`. Imported declarations are accessed as `lexer.Token` or
`lex.tokenize(...)`. Bindings are local to the importing source file; imports never expose unqualified declarations.
Grouped imports remain supported. File imports, angle imports and dot imports are removed. Cycles and invalid `internal`
imports are rejected.

Declarations and members are package-private by default. `pub` exports functions, structs, enums, traits, constants,
variables, fields, methods and enum variants. Private declarations are visible to all files of their package. Types
remain nominal across package and module boundaries. See [MODULE_SYSTEM.md](MODULE_SYSTEM.md) for manifest syntax,
vendor dependencies, visibility, identities and diagnostics.

## Types

The primitive types are `int`, `char`, `byte`, `bit`, `float`, `double`, `string`,
`i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`, `isize`, `usize`, and `void`. Fixed-width integer names denote
their signedness and width; `isize`/`usize` are signed/unsigned pointer-width integers (64 bits on both supported
targets). Existing `int`, `char` and `byte` retain their 32-bit signed, 8-bit signed and 8-bit unsigned memory/ABI
representations. The new spellings are distinct primitive types for overload resolution and generic specialization.
`void` is valid only as a function return type. Fixed arrays use `var name:type[length];`. Repeated prefix stars form
pointers, such as `**int`; grouped types distinguish `*(int[4])` from `*int[4]`. Whole-array assignment is not
supported.

Fixed-size array parameters such as `values:float[2]` accept arrays with exactly the declared length and element type.
They borrow the caller's storage, so element mutations are visible to the caller. Their ABI passes one data pointer;
indexing uses the statically known bound. A slice does not implicitly convert to a fixed-size array parameter.

`T[]` slices are borrowed views containing a data pointer and an element count. They may be parameters, local bindings,
fields, enum payloads, package variables and return values. A matching fixed array converts to a slice without copying
its elements. Assignment and return copy the view; they do not transfer ownership or extend the underlying storage
lifetime. Package variables may be zero-initialized slices; runtime package initializers remain unsupported.
`.length:usize` and `.data:*T` are read-only properties. Indexing checks the current count, including negative indices.
Slice equality is not defined.

`slice(pointer, count)` constructs a view from a typed non-void raw pointer and an integral count. Zero permits a null
pointer; negative counts, nonzero counts with null pointers and byte-size overflow trap. The caller ensures the region
is valid, aligned and alive. Slice parameters use pointer and length ABI lanes in source-parameter order; stored and
returned slices use a two-word descriptor.

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

## Control flow and expressions

DMM supports blocks, `if`/`else`, `for`, `while`, `break`, `continue`, and `return`. `break` and `continue` are valid
only in loops. Every reachable path of a non-void function must return a value of the declared type.

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

Struct arguments, assignments, and return values have by-value semantics, including nested structs and fixed arrays
contained in structs. Struct types are nominal and cannot be assigned merely because their layouts match.

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

`trait Printable { func toString() -> string; }` declares signatures;
`impl Printable for Person { func toString() -> string { return name; } }`
provides an explicit implementation on a nominal struct. `Self` in signatures denotes that struct. Bounds such as
`T:Printable + Equal` require every named implementation; matching methods alone do not satisfy a bound. Calls dispatch
statically after specialization. Missing methods, signature mismatches and conflicting implementations are errors. Trait
inheritance, associated types, default methods and trait objects are not supported.

A sum enum gives each variant its own payload types. Construct values with
`Option<int>.Some(42)` or `Option<int>.None`. The representation stores a tag followed by storage for the largest
variant. Assignment, arguments and returns copy the complete aggregate; copying pointers or strings retains explicit
ownership without destruction. Sum values do not support implicit equality or direct payload member access. For a
variant with exactly one payload, `value.Variant()` returns that payload:
`result.Ok()`, `result.Err()`, and `option.Some()` are instance accessors with no arguments. The result has the
specialized payload type. The receiver is evaluated once, and accessing a variant that is not active traps before
reading its payload. Variants with zero or multiple payloads require `match`. Constructors remain type-qualified, for
example `Result<int,string>.Ok(42)`.

`match (value) { Some(v) => return v; None => return fallback; }` is a statement. Variant names are relative to the
scrutinee enum. Bindings have the exact payload types and are scoped to their arm. Every variant must be covered unless
`_` provides a wildcard arm. Duplicate variants, wrong binding counts and unreachable arms are errors. Payload
extraction is emitted only in a branch guarded by the corresponding tag test; typed IR verifies that guard. An invalid
runtime tag traps.

`import "stdlib";` exports `stdlib.Option<T>`, `stdlib.Result<T,E>`, `stdlib.Cell<T>` with `get`/`set` methods,
`unwrapOr`, `Printable`, `Equal`, and `printValue`. See `tests/execution/generics` for executable examples.

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

`expression.type` accesses the static type of an expression without evaluating it; for example, `value.type.name` or
`make_value().type.size`. Type metadata cannot be stored as a runtime value.

Use `match (T)` or `match (value.type)` with `case Type -> ...` arms to select code at compile time. Only the selected
arm is analyzed for a concrete specialization, so other arms may use operations specific to their own types.
`case _ -> ...` supplies a fallback. Type patterns distinguish primitive types, pointer shapes, array lengths, and
generic specializations. Enum value matches retain their existing syntax and behavior.

Generic functions accept explicit type arguments: `identity<i32>(value)` or `core.alloc<Node>()`. Arguments must satisfy
the function's arity, parameter types and trait bounds. Existing inference remains available when value parameters
determine every type argument; a return type alone does not infer one.

`reserve(type)` zero-initializes one complete sized non-void object and returns a pointer to that type. It accepts a
type rather than a runtime count. `free(value)` releases a raw pointer or owned string, never a slice descriptor or
fixed array directly. Explicit pointer-to-pointer casts such as `data.(*Node)` reinterpret the address; validity and
alignment remain caller responsibilities. Allocations require explicit releases; `@gc` and automatic function-exit
cleanup have been removed.

`stdlib/core` implements `alloc<T>() -> *T`, `alloc<T>(count:usize) -> T[]`, `null<T>() -> *T`, `release<T>(pointer:*T)`
and `release<T>(view:T[])` in DMM over byte-region primitives. Successful allocations are zero-initialized; failure
returns null or an empty null slice. Release an allocation base exactly once, and never release a borrowed view into
local, global or interior storage. See `CORE_RUNTIME.md`.

Types support one array or slice constructor, with pointer levels inside and outside it. Nested arrays and slices are
not supported.

`stdlib.print(value)` and `stdlib.println(value)` are ordinary overloaded stdlib functions. Import `"stdlib"` to use
them; an empty line is `stdlib.println("")`. Calls evaluate their arguments before entering the output function.

`"stdlib/core"` exposes low-level byte-region allocation, explicit release, raw I/O and process primitives without
importing higher-level library functions. See [CORE_RUNTIME.md](CORE_RUNTIME.md) for signatures and ownership contracts.

`"stdlib/stdio"` provides synchronous byte-oriented file and standard streams, buffered readers/writers,
complete-transfer loops, bounded owned byte/line reads and decimal integer output. Stream and buffer owners require
explicit close or release and must not be copied. EOF, partial progress and OS errors are reported as typed library
results. See [STDIO.md](STDIO.md) for the API and lifecycle rules.

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
