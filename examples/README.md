# DMM examples

These programs are executable documentation for the current `2026-09-22-dev` edition. Every directory is an independent
module with its own `dmm.manifest`; together they cover the implemented language areas without relying on planned syntax.

From the repository root:

```sh
./build/compiler examples/language_tour/language_tour.dmm -o language_tour
./language_tour
```

On Windows, use `build/compiler.exe` and an `.exe` output name. Substitute another configured build directory when
needed. `file_io` creates `dmm-example-output.txt` in the process working directory; the other examples only write to
standard output.

## Suggested reading order

| Example | What it demonstrates |
|---|---|
| [language_tour](language_tour/language_tour.dmm) | A score report using constants, primitives, structs, methods, overloads, fixed arrays, slice borrowing, loops, casts and type metadata |
| [callable_values](callable_values/callable_values.dmm) | A transformation pipeline with function-typed fields and arrays, enum payloads, returned callables, equality and unbound methods |
| [sum_types](sum_types/sum_types.dmm) | Success, absence and failure modeled with generic sum enums, exhaustive `match`, `Option` and `Result` |
| [generics_and_interfaces](generics_and_interfaces/generics_and_interfaces.dmm) | Generic containers and functions, multiple structural bounds, static specialization, dynamic interface slices and type matches |
| [memory_and_slices](memory_and_slices/memory_and_slices.dmm) | Nested arrays, checked views and subslices, generated backing storage, raw slices, pointers, `reserve`/`free`, `sizeof` and `alignof` |
| [ownership_and_borrows](ownership_and_borrows/ownership_and_borrows.dmm) | Copy versus move, destructor propagation through generics, reinitialization, disjoint mutable borrows and reverse field drop |
| [defer_cleanup](defer_cleanup/defer_cleanup.dmm) | LIFO `defer`, eager call capture, anonymous-body reference capture and cleanup on return, `continue` and `break` |
| [owning_collections](owning_collections/owning_collections.dmm) | Move-only `Bytes`, `Buffer<T>`, `List<T>` and `String`, including growth, borrowed views and automatic destruction |
| [packages](packages/packages.dmm) | A public API in a second package, an import alias, module-relative resolution, runtime package initialization and package-owner cleanup after `main` |
| [file_io](file_io/file_io.dmm) | A deterministic file round-trip with streams, transfer statuses, byte views and explicit cleanup of current I/O wrappers |

## Coverage map

| Language area | Primary example |
|---|---|
| Declarations, expressions and control flow | `language_tour` |
| Function types and higher-order code | `callable_values` |
| Enums and pattern matching | `sum_types` |
| Generics, interfaces and compile-time type selection | `generics_and_interfaces` |
| Arrays, slices, pointers and raw allocation | `memory_and_slices` |
| Moves, borrows and destructors | `ownership_and_borrows` |
| Scope-exit cleanup | `defer_cleanup` |
| Standard owning collections | `owning_collections` |
| Modules, packages and visibility | `packages` |
| Files, streams and status-based error handling | `file_io` |

Each feature is explained where it is exercised rather than repeated here as a second partial program. Normative rules
live in the [language specification](../LANGUAGE_SPEC.md); diagnostics and edge cases belong in the test suite. Planned
closures, propagation syntax and expression-valued control flow are deliberately absent until their semantics land.
