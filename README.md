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
./build/compiler tests/hello.dmm
gcc -no-pie tests/hello.dmm.s -o hello
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

- native execution of the language corpus in both syntax modes;
- C ABI interoperability;
- semantic and syntax rejection cases;
- large-source and code-capacity stress cases;
- target calling-convention and runtime array-bounds checks;
- focused optimizer, syntax-converter, JSON-diagnostic, and CLI tests.

Test labels can select a subset, for example `ctest --test-dir build -L unit`.

## Documentation

- [Formal language](FORMAL_LANGUAGE.md): lexical and syntactic EBNF.
- [Language specification](LANGUAGE_SPEC.md): semantic rules and implementation limits.
- [Architecture](ARCHITECTURE.md): compiler pipeline, module ownership, backend, and validation design.

## License

MIT. See [LICENSE](LICENSE).
