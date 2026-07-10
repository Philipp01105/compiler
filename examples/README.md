# DMM examples

This directory contains small programs for the current `2026-09-22-dev` edition. Each example focuses on one coherent
part of the language and has its own `dmm.manifest`.

From the compiler repository root:

```sh
./build/compiler examples/language_tour/language_tour.dmm -o language_tour
./language_tour
```

On Windows, use `compiler.exe` and an `.exe` output name. Substitute the configured build directory, such as
`cmake-build-debug`, for `build`.

## Catalog

| Example | Demonstrates |
|---|---|
| [language_tour](language_tour/language_tour.dmm) | Constants, structs, methods, overloads, contextual array literals, slices, postfix casts, loops and type metadata |
| [generics_and_interfaces](generics_and_interfaces/generics_and_interfaces.dmm) | Generic functions and structs, inferred specialization, structural interface implementation, bounds and interface arrays |
| [sum_types](sum_types/sum_types.dmm) | Generic sum enums, qualified constructors, payload bindings, exhaustive `match`, `Option` and `Result` |
| [type_derived_ownership](type_derived_ownership/type_derived_ownership.dmm) | Destructor-derived ownership, generic inference patterns, moves, complete reinitialization, field-sensitive mutable borrows and reverse field drop |
| [package_cleanup](package_cleanup/package_cleanup.dmm) | Exactly-once destruction of package values in reverse declaration order after normal `main` return |
| [defer_cleanup](defer_cleanup/defer_cleanup.dmm) | LIFO `defer`, eager call-argument capture, anonymous-body reference capture and return cleanup |
| [memory_and_slices](memory_and_slices/memory_and_slices.dmm) | Fixed arrays, owned literal backing for non-owning slices, borrowed slices, checked-borrow-to-pointer casts, `reserve`/`free`, `sizeof` and `alignof` |
| [io_demo](io_demo/io_demo.dmm) | Buffered standard input, stream output, file writing/read-back, byte slices and explicit wrapper cleanup |

## Generic inference: whole type versus inner type

The parameter pattern determines what a type parameter represents:

```dmm
func consumeValue<T>(value:T) -> void {}
func consumeBox<T>(value:Box<T>) -> void {}

var file:Box<File>;
consumeValue(file); // T = Box<File>

var other:Box<File>;
consumeBox(other);  // T = File; the parameter itself is Box<File>
```

The ownership example prints `T.name` for both calls and `value.type.name` for the shaped parameter. This makes both
the inferred inner type and the retained complete `Box<File>` parameter type directly observable.

## Ownership shown by the examples

All user-defined records use normal `struct` syntax. A destructor makes its type `MOVE_ONLY | NEEDS_DROP`; those
properties propagate through fields and concrete generic specializations. Passing or assigning a move-only value by
value transfers ownership. The source cannot be used again until a complete assignment reinitializes it.

Compiler-generated cleanup runs once for every live value that needs drop. A struct destructor runs before owned
fields, fields are dropped in reverse declaration order, and normal process termination also cleans package storage.
Package-owned values cannot be moved out.

Raw allocation and the current stream wrappers remain explicitly managed. `memory_and_slices` calls `free`, while
`io_demo` calls `close`, `discard`, and `release` on all relevant paths because those library wrappers do not yet
declare destructors.

`io_demo` is interactive. It asks for a name and message, writes `demo_output.txt`, then streams the file back to
standard output. The remaining examples run without input.
