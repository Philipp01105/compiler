# DMM Compiler

DMM is an experimental compiler for a small, statically typed language with generics, checked borrows, type-derived
ownership and deterministic cleanup. It emits x86-64 assembly, ELF/COFF objects and internally linked ELF/PE
executables for Linux and Windows.

DMM is suitable for language and compiler experimentation. It is not yet intended for production workloads, and the
current `2026-09-22-dev` edition does not promise backwards compatibility.

## Quick start

Requirements:

- CMake 3.21 or newer
- a C23 compiler
- GCC-compatible tools only when using GNU assembly output or the interoperability tests

Configure, build and test:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Compile and run an example on Linux:

```sh
./build/compiler examples/language_tour/language_tour.dmm -o language_tour
./language_tour
```

On Windows, use `build/compiler.exe` and an `.exe` output name. Multi-configuration generators may place the compiler
under `build/Debug` or `build/Release`.

Every DMM project has a `dmm.manifest` and every source file begins with a package declaration:

```text
module example.com/hello
dmm 2026-09-22-dev
```

```dmm
package main;
import "stdlib";

enum Lookup<T> {
    Found(T),
    Missing,
}

struct Counter {
    var value:int;

    func add(amount:int) -> void {
        value += amount;
    }
}

func sum(values:int[]) -> int {
    var total:int = 0;
    for (var index:int = 0; index < values.length; index += 1) {
        total += values[index];
    }
    return total;
}

func main() -> int {
    var counter:Counter;
    counter.add(sum([2,4,6,8]));

    var result:Lookup<int> = Lookup<int>.Found(counter.value);
    match (result) {
        Found(value) => stdlib.println(value);
        Missing => return 1;
    }
    return 0;
}
```

The example shows a generic sum type, a method, a contextually typed slice literal, an exhaustive match and ordinary
control flow. The [example catalog](examples/README.md) covers ownership, checked borrows, `defer`, interfaces, package
cleanup, raw memory and stream I/O in focused programs.

## Implemented language areas

- primitive and fixed-width numeric types, pointers, fixed arrays, slices and contextual array/slice literals
- functions, overloads, non-capturing callable values, structs, methods, enums and exhaustive `match`
- generic functions, structs and enums with interface bounds and specialization
- structural interfaces with owned values, move-only implementers and dynamic dispatch
- type-derived `COPYABLE`, `MOVE_ONLY` and `NEEDS_DROP` properties, implicit moves and deterministic destruction
- checked `&T` and exclusive `&mut T` borrows with field-sensitive conflicts and conservative last-use lifetimes
- LIFO `defer`, including eager call capture and anonymous deferred bodies
- expression-valued `if`, blocks and exhaustive `match`, plus `Option`/`Result` propagation with `?`
- sequentially consistent `AtomicBit`/`AtomicUsize` and experimental async futures, executors and confirmed cancellation
- typed asynchronous TCP/UDP and DNS through `stdlib/core/net`, with an automatic external-linker handoff
- packages, explicit exports, local vendored dependencies and deterministic manifest synchronization
- verified typed IR, `-O0`/`-O1`, native ELF/COFF object emission and internal ELF/PE linking
- native `extern "system"` FFI for x86-64 Linux and MinGW-w64 UCRT64, including C-layout structs by value, symbol aliases and logical library overrides

The [language specification](LANGUAGE_SPEC.md) is authoritative for semantics. The [roadmap](TODO.md) distinguishes
implemented behavior from planned features.

Async programs require `features = ["async"]` in the root `dmm.manifest`. An `async func` call constructs a lazy,
move-only `Future<T>`; use `future.await()` inside async code and `block_on(future)` from synchronous code.
`spawn(future)` requires a concrete Send future and returns a consuming `JoinHandle<T>`. Futures, joins and executor
owners must be consumed on every path; cancellation completes through an awaited operation. See the
[language specification](LANGUAGE_SPEC.md) and [executor contract](src/runtime/EXECUTOR.md).

## Compiler use

The compiler accepts a source file or package directory. Executable output is the default:

```sh
./build/compiler path/to/main.dmm -o program
./build/compiler -c --target=coff path/to/main.dmm -o program.obj
./build/compiler -S --syntax=att --target=elf path/to/main.dmm
```

Run `compiler --help` for the current option list. Useful groups include:

- `--emit=exe|obj|asm`, `-c`, `-S`, `-o FILE`
- `--link=auto|internal|external`, `--linker-driver PATH`, `--runtime-shim PATH` (private platform runtime)
- `-O0`, `-O1`, `--target=elf|coff`, `--syntax=intel|att`
- `--formatError`, `--ide`, `--ide-buffer FILE`
- `--dump-tokens`, `--dump-ast`, `--dump-symbols`, `--dump-ir-before-opt`, `--dump-ir-passes`, `--dump-ir`,
  `--dump-cfg` and `--source-map`
- `--deterministic`, `--version`

Use `dmm manifest sync [package-directory]` to reconcile direct and indirect local dependencies. It never fetches
packages or changes dependency versions.

Standalone native executables do not require libc, a CRT or a foreign-language runtime. Linux targets use syscalls; Windows
targets import OS APIs. Assembly output can be handed to external GNU-compatible tools when required. The exact target
and linking contracts are documented in [NATIVE_BACKEND.md](NATIVE_BACKEND.md).

The optional `--link=external` profile uses regular platform startup and a private platform shim, so those programs
gain libc/UCRT and documented OS dependencies. Default standalone builds retain their existing runtime and internal
linker. For object/assembly emission this option selects the future link ABI and starts no external process.
Used operations from `stdlib/core/net` select that profile automatically and choose the combined network shim.
TCP/UDP, IPv4/IPv6, deadlines, cancellation and bounded DNS are documented in
[NETWORK_RUNTIME.md](NETWORK_RUNTIME.md); an unused import introduces no networking dependency.

## Tests and CI

CTest covers execution, rejection diagnostics, runtime traps, ABI interoperability, regression cases, stress limits,
unit tests, dump formats, imports, native linking and architecture boundaries. Select a subset with a label, for
example:

```sh
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build -L async --output-on-failure
ctest --test-dir build -L network --output-on-failure
```

CI currently runs Linux/GCC, Linux/Clang, Linux/GCC with ASan+UBSan, Linux/Clang libFuzzer smoke tests, Windows/MinGW
GCC and Windows/Clang. Optional runtime benchmarks and standalone fuzzing commands have dedicated documents.

## Documentation

Each topic has one canonical document; other documents link to it instead of repeating its contract.

| Document | Purpose |
|---|---|
| [Examples](examples/README.md) | Runnable feature-oriented programs and suggested reading order |
| [Formal language](FORMAL_LANGUAGE.md) | Lexical and syntactic EBNF |
| [Language specification](LANGUAGE_SPEC.md) | Source-language semantics and implementation limits |
| [Modules](MODULE_SYSTEM.md) | Manifests, package discovery, imports, visibility and vendored dependencies |
| [Core runtime](CORE_RUNTIME.md) | Low-level memory, I/O and process boundary |
| [Executor runtime](src/runtime/EXECUTOR.md) | Pinned frames, scheduling, cancellation and private platform ABI |
| [Network runtime](NETWORK_RUNTIME.md) | Typed TCP/UDP/DNS, runtime selection, loans and confirmed completion |
| [Streams and buffered I/O](STDIO.md) | `stdlib/stdio` API, ownership and error contracts |
| [Architecture](ARCHITECTURE.md) | Compiler pipeline and component responsibilities |
| [IR optimization](IR_OPTIMIZATION.md) | Optimization passes and preserved semantic effects |
| [Native backend](NATIVE_BACKEND.md) | x86-64 ABI, object formats, runtime profiles and linker handoff |
| [Dump formats](DUMP_FORMATS.md) | Versioned tokens, AST, symbols, IR, CFG and source maps |
| [Diagnostics audit](DIAGNOSTICS_AUDIT.md) | Diagnostic families, rendering contract and validation |
| [Fuzzing](FUZZING.md) | libFuzzer targets, corpora and reproduction workflow |
| [Performance](PERFORMANCE.md) | Opt-in runtime benchmarks and baseline comparisons |
| [Roadmap](TODO.md) | Planned work, priorities and definition of done |

## License

MIT. See [LICENSE](LICENSE).
