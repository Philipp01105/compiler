# DMM Language Specification 0.1

Status: experimental. This document defines the tested source-language contract; undocumented behavior may change.

## Source and declarations

A source file is UTF-8 text containing imports, structs, enums, traits, implementations, constants, and functions. Execution begins in a parameterless `main`, whose return type is `void` or `int`. Statements end with `;`. `//` introduces a line comment.

Imports use `import "relative/path.dmm"` or `import <relative/path.dmm>`, individually or grouped as `import ( path ... )`. Directory imports load only `package.dmm` and its explicit imports. Canonical files are loaded once; unlisted files are never scanned.

## Types

The primitive types are `int`, `char`, `byte`, `bit`, `float`, `double`, `string`, and `void`. `void` is valid only as a function return type. Fixed arrays use `var name:type[length];`. Repeated prefix stars form pointers, such as `**int`; grouped types distinguish `*(int[4])` from `*int[4]`. Whole-array assignment is not supported.

Parameters may use `T[]` slices. A matching fixed array supplies a data pointer and element count without copying; forwarding a slice preserves both. `slice.length` is read-only and indexing checks the passed count. The native ABI expands each slice to pointer then length in source-parameter order. Slices cannot be returned or stored in local bindings or fields.

Implicit conversions preserve their source domain: integral types may convert among themselves or widen to floating point, and `float` may widen to `double`. Narrowing floating conversions require an explicit cast.

Casts use `value.(target)`, for example `(amount / 2.0).(int)` or `value.(byte).(int)`. The target must be a numeric primitive type. The old `int(value)` form is rejected. Casts bind as postfix expressions; parentheses group compound source expressions.

`const name[:type] = expression;` declares a top-level or block constant. Primitive and string initializer expressions are evaluated during semantic analysis. Earlier evaluated constants may be referenced, including positive `int` constants used as fixed-array lengths. Constants have no mutable storage and cannot be assigned, incremented, or addressed. Integer overflow and integer division by zero are compilation errors.

## Control flow and expressions

DMM supports blocks, `if`/`else`, `for`, `while`, `break`, `continue`, and `return`. `break` and `continue` are valid only in loops. Every reachable path of a non-void function must return a value of the declared type.

Operator precedence, from low to high, is logical OR, logical AND, comparisons, addition/subtraction, multiplication/division/remainder, unary operators, and primary expressions. Arithmetic is numeric; `bit` values participate in conditions and logic but not arithmetic. Remainder is defined only for integral operands. Assignment requires a mutable lvalue.

`string + value` concatenates strings with `string`, numeric, `char`, or `bit` values and returns an owned string. The caller may release that result with `free`. Printing accepts scalar and string values; a null string prints as an empty string.

Static array accesses are checked at compile time when the index is a literal and at runtime otherwise. An out-of-bounds runtime access traps.

Address-of accepts variables and array elements. Dereference requires a pointer.
Pointer arithmetic is not part of DMM and is rejected.

Struct arguments, assignments, and return values have by-value semantics,
including nested structs and fixed arrays contained in structs. Struct types are
nominal and cannot be assigned merely because their layouts match.

Legacy enum member names must be unique within an enum. Legacy enum values are first-class:
they can be stored, compared for equality, passed, and returned. Variant fields
are scalar primitive types initialized by compile-time literals and can be read
through member access. Unknown enum members are a compile error.

## Compile-time abstraction and sum types

Functions, structs and enums accept type parameters: `func identity<T>(value:T) -> T`,
`struct Pair<A,B>`, and `enum Option<T> { Some(T), None, }`. Calls infer one consistent
substitution from argument types; the expected return type does not infer missing
arguments. Repeated occurrences of a parameter require the same concrete type.
Generic nominal types are invariant, including pointer levels, fixed-array lengths,
slice elements, and nested nominal arguments. An otherwise-equivalent concrete exact
overload wins over a generic overload. Ambiguous substitutions are rejected.

The compiler caches concrete specializations, with identities derived from the
declaration and its complete type arguments. Each module permits at most 256
specializations, declarations at most 16 type parameters, and aggregate instantiation
nesting at most 64 levels. Encoded specialization identities must fit 4095 bytes.
By-value recursive layouts require a pointer to break the
cycle. Only concrete specializations reach typed IR and native emission.

`trait Printable { func toString() -> string; }` declares signatures;
`impl Printable for Person { func toString() -> string { return name; } }`
provides an explicit implementation on a nominal struct. `Self` in signatures denotes
that struct. Bounds such as `T:Printable + Equal` require every named implementation;
matching methods alone do not satisfy a bound. Calls dispatch statically after
specialization. Missing methods, signature mismatches and conflicting implementations
are errors. Trait inheritance, associated types, default methods and trait objects
are not supported.

A sum enum gives each variant its own payload types. Construct values with
`Option<int>.Some(42)` or `Option<int>.None`. The representation stores a tag followed
by storage for the largest variant. Assignment, arguments and returns copy the complete
aggregate; copying pointers or strings retains explicit ownership without destruction.
Sum values do not support implicit equality or direct payload member access.

`match (value) { Some(v) => return v; None => return fallback; }` is a statement.
Variant names are relative to the scrutinee enum. Bindings have the exact payload
types and are scoped to their arm. Every variant must be covered unless `_` provides
a wildcard arm. Duplicate variants, wrong binding counts and unreachable arms are
errors. Payload extraction is emitted only in a branch guarded by the corresponding
tag test; typed IR verifies that guard. An invalid runtime tag traps.

`<stdlib>` exports `Option<T>`, `Result<T,E>`, `Cell<T>` with `get`/`set` methods,
`unwrapOr`, `Printable`, `Equal`, and `printValue`. See `tests/execution/generics` for executable examples.

Functions declared inside a struct are invoked as instance methods, while
`static func` members are invoked on the struct type. Top-level and method link
names that overlap the runtime are mangled internally, so source-level function
names remain usable without breaking platform calls. Names beginning with
`__dmm_` are reserved.

`reserve(type)` zero-initializes one complete sized non-void object and returns a pointer to that type. It accepts a type rather than a runtime count. `free(value)` releases a pointer or owned string. Allocations require explicit releases; `@gc` and automatic function-exit cleanup have been removed.

Types support one array or slice constructor, with pointer levels inside and outside it. Nested arrays and slices are not supported.

`print(value)` and `println(value)` are ordinary overloaded stdlib functions. Import `<stdlib>` to use them; an empty line is `println("")`. Calls evaluate their arguments before entering the output function.

Function overloads differ by ordered parameter types, never return type. Exact matches beat promotions and other allowed numeric conversions. A candidate must be no worse in every argument and better in at least one; ties are ambiguous. `main` cannot be overloaded. Overloaded functions and methods use type-derived link names.

## Implementation limits

Tokens are at most 511 bytes, an expression is at most 512 tokens, and a local object or function stack frame is at most 8 MiB. Exceeding a limit is a compilation error, never silent truncation.

## Diagnostics and output

Normal diagnostics are human-readable. `--formatError` emits a single JSON document with `errors` and `summary`; each error records its category, code, line, and column. JSON mode emits no ANSI escapes or unrelated output. The compiler produces GNU x86-64 assembly for ELF/System V or COFF/Windows in Intel or AT&T syntax.
