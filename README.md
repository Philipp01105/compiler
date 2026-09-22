# DMM Compiler

DMM is an experimental compiler for a small, statically typed language with generics, checked borrows, type-derived
ownership and deterministic cleanup. It emits x86-64 assembly, ELF/COFF objects and internally linked ELF/PE
executables for Linux and Windows.

DMM is suitable for language and compiler experimentation. It is not yet intended for production workloads, and the
current `2026-10-04-dev` edition also accepts existing `2026-09-22-dev` modules;
this limited compatibility does not establish a stable compatibility guarantee.

## Quick start

Requirements:

- CMake 3.21 or newer
- a C23 compiler
- GCC/Clang and target libraries for native FFI, platform/network runtime linking or interoperability tests;
  GNU-compatible tools for assembling emitted assembly

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
- functions, overloads, explicit capturing closures, shared/mutable/once callable bounds, non-capturing callable values, structs with named value initializers, methods, enums and exhaustive `match`
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
- native function pointers and exported callbacks, unions, packing/alignment, target-specific source files and raw OS bindings in `stdlib/native`

The [language specification](LANGUAGE_SPEC.md) is authoritative for semantics. The [roadmap](TODO.md) distinguishes
implemented behavior from planned features.

Async programs require `features = ["async"]` in the root `dmm.manifest`. An `async func` call constructs a lazy,
move-only `Future<T>`; use `future.await()` inside async code and `block_on(future)` from synchronous code.
`spawn(future)` requires a concrete Send future and returns a consuming `JoinHandle<T>`. Futures, joins and executor
owners must be consumed on every path; cancellation completes through an awaited operation. See the
[language specification](LANGUAGE_SPEC.md) and [executor contract](ARCHITECTURE.md#executor-runtime-and-native-abi).

## Compiler use

The compiler accepts a source file or package directory. Executable output is the default:

```sh
./build/compiler path/to/main.dmm -o program
./build/compiler -c --target=coff path/to/main.dmm -o program.obj
./build/compiler -S --syntax=att --target=elf path/to/main.dmm
```

Run `compiler --help` for the current option list. Useful groups include:

- `--emit=exe|obj|asm`, `-c`, `-S`, `-o FILE`
- `--link=auto|internal|external`, `--linker-driver PATH`, `--runtime-shim PATH` (optional additional native runtime object)
- `--native-library NAME=PATH`, repeatable `--native-library-dir DIR`, `--dump-native-link FILE`
- `-O0`, `-O1`, `--target=elf|coff`, `--syntax=intel|att`
- `--formatError`, `--ide`, `--ide-buffer FILE`
- `--dump-tokens`, `--dump-ast`, `--dump-symbols`, `--dump-ir-before-opt`, `--dump-ir-passes`, `--dump-ir`,
  `--dump-cfg` and `--source-map`
- `--deterministic`, `--version`

Use `dmm manifest sync [package-directory]` to reconcile direct and indirect local dependencies. It never fetches
packages or changes dependency versions.

Standalone native executables do not require libc, a CRT or a foreign-language runtime. Linux targets use syscalls; Windows
targets import OS APIs. Assembly output can be handed to external GNU-compatible tools when required. The exact target
and linking contracts are documented in [ARCHITECTURE.md](ARCHITECTURE.md#native-x86-64-backend).

The optional `--link=external` profile uses regular platform startup and a DMM platform runtime, so those programs
gain libc/UCRT and documented OS dependencies. Emitted async bodies and executor operations select this profile.
Programs without async or FFI retain the standalone runtime and internal
linker. For object/assembly emission this option selects the future link ABI and starts no external process.
Used operations from `stdlib/core/net` select that profile automatically and choose the network archive containing
DMM platform, executor and networking code. OS calls use ordinary FFI bindings; C runtime implementations are
kept only as independent test references.
TCP/UDP, IPv4/IPv6, deadlines, cancellation and bounded DNS are documented in
[stdlib/core/net/README.md](stdlib/core/net/README.md); an unused import introduces no networking dependency.

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
GCC and Windows/Clang. The following optional workflows complement the normal CTest suite.

### Performance

Enable benchmarks with `-DDMM_ENABLE_PERF_TESTS=ON` and run
`ctest --test-dir build -R '^runtime_performance$' --output-on-failure`. For a standalone report:

```sh
python tests/performance/run.py --compiler build/compiler --output-dir build/performance --warmups 2 --samples 7
```

The harness checks results and writes medians to `report.json`. Compare a previous report with
`--baseline previous-report.json --max-regression-percent 10`; regression limits apply to optimized results.
Use the same target, compiler configuration and idle hardware for comparable measurements. Default CTest sets no
timing threshold.

### Fuzzing

On a Unix-like host with Clang and libFuzzer:

```sh
cmake -S . -B build-fuzz -DCMAKE_C_COMPILER=clang -DDMM_BUILD_FUZZERS=ON -DBUILD_TESTING=OFF
cmake --build build-fuzz --parallel
./build-fuzz/fuzz_parser writable-corpus -dict=tests/fuzz/dmm.dict -max_total_time=60
```

Targets are `fuzz_lexer`, `fuzz_parser`, `fuzz_semantic`, `fuzz_ir` and `fuzz_syntax_converter`. Copy seed corpora from
`tests/fuzz/corpus` into a writable directory before running; the converter can start with an empty corpus. Source
inputs are capped at 64 KiB, diagnostics are buffered and imports cannot read arbitrary filesystem paths. The IR
harness also mutates instructions and reruns verification. `fuzz_pipeline_smoke` runs in the normal test suite;
CI runs each libFuzzer target for a short bounded interval.

## Documentation

Each topic has one canonical document; other documents link to it instead of repeating its contract.

| Document | Purpose |
|---|---|
| [Language specification](LANGUAGE_SPEC.md) | Semantics, modules, manifests, native FFI and EBNF |
| [Architecture](ARCHITECTURE.md) | Pipeline, optimization, backend, runtime contracts, dumps and diagnostics |
| [Standard library](stdlib/README.md) | Package index, common value types, collections and compatibility I/O |
| [Core](stdlib/core/README.md) | Memory, allocation, atomics, descriptors and process primitives |
| [Streams](stdlib/stdio/README.md) | Synchronous streams, buffers and transfer/error contracts |
| [Networking](stdlib/core/net/README.md) | TCP/UDP/DNS, ownership, deadlines, cancellation and lifecycle |
| [Native bindings](stdlib/native/README.md) | Raw OS/CRT bindings and caller responsibilities |
| [Examples](examples/README.md) | Runnable feature-oriented programs |
| [FFI and runtime migration](plans/ffi.md) | Completed stages 1–4 and remaining network migration |
| [Roadmap](TODO.md) | Planned work and priorities |

## License

MIT. See [LICENSE](LICENSE).
