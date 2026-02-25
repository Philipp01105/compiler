# DMM Compiler

DMM is an experimental compiler for a small statically typed language. It emits x86-64 GNU assembly in Intel or AT&T syntax and supports ELF/System V and COFF/Windows calling conventions.

The project is suitable for learning and experimentation. It is not yet intended for production workloads.

## Supported features

- Primitive types: `int`, `char`, `byte`, `bit`, `float`, `double`, `string`, and `void`
- Functions, forward calls, local variables, arrays, pointers, structs, enums, and methods
- `if`/`else`, `for`, `while`, `break`, `continue`, and `return`
- String operations, formatted input/output, file I/O, heap allocation, and garbage-collected allocations
- Source imports through `# import`
- Linux ELF and Windows COFF assembly output
- Intel syntax by default, with AT&T syntax available
- Human-readable or JSON diagnostics
- An owned AST boundary separating frontend input handling from backend emission

## Requirements

- CMake 3.21 or newer
- A C23 compiler
- GCC or a compatible GNU assembler/linker to assemble generated `.s` files

The automated suite is exercised with GCC on Linux and MinGW-w64 on Windows.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Enable warnings as errors with `-DDMM_STRICT_WARNINGS=ON`.

## Compile a program

```sh
./build/compiler tests/execution/basics/hello.dmm
gcc -no-pie tests/execution/basics/hello.dmm.s -o hello
./hello
```

On a multi-configuration generator, the compiler executable may be inside `build/Debug` or `build/Release`.

Useful options:

```text
--syntax=intel|att
--target=elf|coff
--formatError
--tokens
--debug
--deterministic
-o FILE
--version
```

`--deterministic` omits the generation timestamp. `--formatError` emits one JSON document containing an `errors` array and a `summary` object.

## Tests

CTest includes:

- `tests/execution/`: happy paths and edge cases with exact expected output;
- `tests/rejection/`: individual lexer, parser, semantic, and type failures;
- `tests/runtime_failure/`: programs that must compile and then trap;
- `tests/abi/`: native ABI boundaries and C interoperability;
- `tests/regression/`: minimal reproductions of fixed compiler bugs;
- `tests/stress/`: generated `limit-1`, `limit`, and `limit+1` cases;
- `tests/unit/`: lexer, optimizer, and syntax-converter unit tests;
- architecture and AST ownership tests that enforce layer boundaries;
- `tests/fuzz/`: libFuzzer entry points for the lexer and syntax converter.

Every normal DMM execution test is compiled, assembled, and run in both Intel
and AT&T syntax. Both runs must have exit code zero, empty stderr, and output
identical to the checked-in `.expected` file. This makes syntax equivalence a
standard property of every execution test rather than a separate smoke test.

Test labels can select a subset, for example `ctest --test-dir build -L unit`.
On Linux, `-DDMM_SANITIZERS=ON` instruments the compiler and unit tests and also
links generated execution/ABI programs against ASan/UBSan runtimes. CI runs the
`safety` label with leak detection enabled.

Clang users can build both fuzzers with:

```sh
cmake -S . -B fuzz-build -DDMM_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang
cmake --build fuzz-build --target fuzz_lexer fuzz_syntax_converter
```

## Documentation

- [Formal language](FORMAL_LANGUAGE.md): lexical and syntactic EBNF.
- [Language specification](LANGUAGE_SPEC.md): semantic rules and implementation limits.
- [Architecture](ARCHITECTURE.md): compiler pipeline, module ownership, backend, and validation design.

Source code is grouped by ownership under `src/common`, `src/ast`,
`src/frontend`, `src/backend`, `src/diagnostics`, and `src/driver`.

## License

MIT. See [LICENSE](LICENSE).
