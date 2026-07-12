# DMM examples

These programs are executable documentation for the current `2026-09-22-dev` edition. Each directory is an independent
module with its own `dmm.manifest`, and each program focuses on a small set of related features.

From the repository root:

```sh
./build/compiler examples/language_tour/language_tour.dmm -o language_tour
./language_tour
```

On Windows, use `build/compiler.exe` and an `.exe` output name. Substitute another configured build directory when
needed.

## Suggested reading order

| Example | Main ideas |
|---|---|
| [language_tour](language_tour/language_tour.dmm) | Constants, structs, methods, overloads, contextual literals, slices, loops, casts and type metadata |
| [callable_values](callable_values/callable_values.dmm) | Function types, higher-order calls, returned callables, callable equality and unbound methods |
| [sum_types](sum_types/sum_types.dmm) | Generic sum enums, qualified constructors, payload bindings, exhaustive `match`, `Option` and `Result` |
| [generics_and_interfaces](generics_and_interfaces/generics_and_interfaces.dmm) | Generic functions and structs, inferred specialization, interface bounds and dynamic interface collections |
| [memory_and_slices](memory_and_slices/memory_and_slices.dmm) | Fixed arrays, slice views, raw allocation, pointer casts, `sizeof` and `alignof` |
| [type_derived_ownership](type_derived_ownership/type_derived_ownership.dmm) | Copy versus move, destructor propagation, reinitialization, checked mutable borrows and reverse field drop |
| [defer_cleanup](defer_cleanup/defer_cleanup.dmm) | LIFO `defer`, eager call capture, anonymous-body reference capture and cleanup on return |
| [package_cleanup](package_cleanup/package_cleanup.dmm) | Exactly-once package-owner destruction after a normal return from `main` |
| [io_demo](io_demo/io_demo.dmm) | Streams, buffered I/O, files, byte slices and explicit cleanup of current I/O wrappers |

## Feature snapshots

Context supplies the element type of an array or slice literal:

```dmm
func sum(values:int[]) -> int { /* ... */ }

var fixed:int[4] = [2,4,6,8];
var view:int[] = fixed;
var total:int = sum([2,4,6,8]);
```

Generic enum constructors carry their concrete specialization, and `match` must cover every variant:

```dmm
enum Lookup<T> {
    Found(T),
    Missing,
    Failed(int),
}

var result:Lookup<int> = Lookup<int>.Found(42);
match (result) {
    Found(value) => stdlib.println(value);
    Missing => stdlib.println("missing");
    Failed(code) => stdlib.println(code);
}
```

Interfaces are structural: a concrete type implements an interface by providing the required method shape.

```dmm
interface Measurable {
    func measure(scale:int) -> int;
}

struct Width {
    var value:int;
    func measure(scale:int) -> int { return value * scale; }
}

func scaled<T:Measurable>(value:T) -> int {
    return value.measure(2);
}
```

Ownership is derived from concrete fields. The complete runnable example shows why `Box<int>` is copyable while
`Box<File>` moves and is destroyed exactly once:

```dmm
struct File {
    var handle:int;
    destructor { handle = 0; }
}

struct Box<T> {
    var value:T;
}

var number:Box<int>;
var numberCopy = number;

var file:Box<File>;
consumeValue(file); // ownership moves into the parameter
```

Deferred calls capture evaluated operands immediately. Anonymous deferred bodies instead observe referenced locals when
the surrounding scope exits:

```dmm
var digit:int = 1;
defer append(target,digit); // captures 1
defer func() {
    append(target,digit);   // reads digit when the defer runs
}
digit = 2;
```

These snippets are excerpts, not separate fixtures. Follow the links in the catalog for complete programs with imports,
return paths and observable results. Normative rules live in the [language specification](../LANGUAGE_SPEC.md); planned
syntax such as closures and expression-valued `if`/`match` is deliberately absent from the examples.
