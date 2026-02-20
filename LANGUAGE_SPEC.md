# DMM Language Specification 0.1

Status: experimental. This document defines the tested source-language contract; undocumented behavior may change.

## Source and declarations

A source file is UTF-8 text containing imports, structs, enums, and functions. Execution begins in `func main() -> void`. Statements end with `;`. `//` introduces a line comment.

Imports use `#import "relative/path.dmm"` or `#import <relative/path.dmm>`. A resolved file is imported at most once during a compilation.

## Types

The primitive types are `int`, `char`, `byte`, `bit`, `float`, `double`, `string`, and `void`. Arrays use `var [length] name:type;`; pointers use `var name:*type`. Array lengths must be positive compile-time integers and local objects are subject to implementation limits.

Implicit conversions preserve their source domain: integral types may convert among themselves or widen to floating point, and `float` may widen to `double`. Narrowing floating conversions require an explicit cast.

## Control flow and expressions

DMM supports blocks, `if`/`else`, `for`, `while`, `break`, `continue`, and `return`. `break` and `continue` are valid only in loops. Every reachable path of a non-void function must return a value of the declared type.

Operator precedence, from low to high, is logical OR, logical AND, comparisons, addition/subtraction, multiplication/division/remainder, unary operators, and primary expressions. Remainder is defined only for integral operands. Assignment requires a mutable lvalue.

Static array accesses are checked at compile time when the index is a literal and at runtime otherwise. An out-of-bounds runtime access traps.

Address-of accepts variables and array elements. Dereference requires a pointer.
Pointer arithmetic is not part of DMM and is rejected.

Enum member names must be unique within an enum. Unknown enum members are a
compile error.

## Implementation limits

Tokens are at most 511 bytes, an expression is at most 512 tokens, and a local object or function stack frame is at most 8 MiB. Exceeding a limit is a compilation error, never silent truncation.

## Diagnostics and output

Normal diagnostics are human-readable. `--formatError` emits a single JSON document with `errors` and `summary`; each error records its category, code, line, and column. JSON mode emits no ANSI escapes or unrelated output. The compiler produces GNU x86-64 assembly for ELF/System V or COFF/Windows in Intel or AT&T syntax.
