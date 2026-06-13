# DMM Compiler

DMM is an experimental compiler for a small statically typed language. It emits x86-64 assembly, native ELF/COFF
objects, and ELF/PE executables with an internal linker for Linux and Windows.

The project is suitable for learning and experimentation. It is not yet intended for production workloads.

## Supported features

- Primitive types: `int`, `char`, `byte`, `bit`, `float`, `double`, `string`, and `void`
- Signed/unsigned fixed-width integers `i8`/`u8` through `i64`/`u64`, and `isize`/`usize`
- A standalone `stdlib/core` package for byte regions, raw I/O and process primitives
- DMM stream I/O in `stdlib/stdio`, with buffered readers/writers, complete transfers and explicit errors;
  see [STDIO.md](STDIO.md)
- Functions, forward calls, local variables, arrays, pointers, structs, enums, and methods
- Inferred and explicitly instantiated generic functions, and invariant generic structs/enums
- Compile-time `sizeof(T)` / `alignof(T)` and typed `core.alloc<T>()` / `core.alloc<T>(count)`
- Compile-time type `.name` / `.size` / `.align`, unevaluated `expression.type`, and `case Type ->` matches
- Borrowed slices usable in variables, fields, enum payloads and returns, with checked indexing
- Explicit traits, `Self`, multiple trait bounds, and static method dispatch
- Tagged variant payloads and exhaustive `match` statements with typed bindings
- `if`/`else`, `for`, `while`, `break`, `continue`, and `return`
- String operations, formatted input/output, file I/O, and explicit heap allocation/free
- Modules with `dmm.manifest`, directory packages, qualified imports and explicit `pub` exports
- Local versioned vendor dependencies, forbidden import cycles and restricted `internal` packages
- Linux ELF and Windows COFF objects, ELF/PE executables, and GNU assembly output
- Intel syntax by default, with AT&T syntax available
- Human-readable or JSON diagnostics
- A recursive owned AST, independent semantic model, and verified typed IR separating frontend analysis from backend
  emission

## Requirements

- CMake 3.21 or newer
- A C23 compiler
- GCC or compatible GNU tools for assembly mode and object interoperability tests

Native executable mode requires no external build tools at compilation time. Generated programs target x86-64 Linux or
Windows without libc, CRT or a foreign language runtime. Linux uses direct syscalls; Windows imports only OS APIs.

The automated suite is exercised with GCC on Linux and MinGW-w64 on Windows.

## Build

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Enable warnings as errors with `-DDMM_STRICT_WARNINGS=ON`.

## Compile a program

Each DMM project needs a `dmm.manifest`; each source file starts with `package name;`. The compiler accepts a source
file or package directory. See [MODULE_SYSTEM.md](MODULE_SYSTEM.md)
for package discovery, visibility, imports and local vendor dependencies.

The build also provides `build/dmm` (`build/dmm.exe` on Windows). Run
`dmm manifest sync` from a project package to synchronize direct and indirect dependencies in `dmm.manifest` and remove
unused requirements.

Emit and internally link a native executable:

```sh
./build/compiler tests/execution/basics/hello/hello.dmm -o hello
./hello
```

On Windows use `-o hello.exe`. Use `-c` or `--emit=obj` for a direct native object. Executable output is the default.
See [NATIVE_BACKEND.md](NATIVE_BACKEND.md) for target formats and linking details.

Assembly output is available with `-S` or `--emit=asm`:

```sh
./build/compiler -S tests/execution/basics/hello/hello.dmm
gcc -nostdlib -no-pie -Wl,-e,__dmm_entry tests/execution/basics/hello/hello.dmm.s -o hello
./hello
```

On a multi-configuration generator, the compiler executable may be inside `build/Debug` or `build/Release`.

Assembly and native object outputs embed the same compiler-owned runtime as internally linked executables. No runtime
archive is needed. On Windows, use
`-nostdlib -Wl,--entry=__dmm_entry,--subsystem,console` and append `-lkernel32`
after the assembly or object input. Sanitizers instrument the C compiler and test harnesses; emitted runtime
instructions are not sanitizer-instrumented.

Useful options:

```text
--emit=exe|obj|asm (default: exe)
-c                  Native object output
-S                  Assembly output
-O0                 Disable IR optimization
-O1                 Enable IR optimization (default)
--syntax=intel|att
--target=elf|coff
--formatError
--tokens
--debug
--deterministic
--dump-tokens FILE
--dump-ast FILE
--dump-symbols FILE
--dump-ir-before-opt FILE
--dump-ir FILE
--dump-cfg FILE
--source-map FILE
-o FILE
--version
```

`--deterministic` omits the generation timestamp. `--formatError` emits one JSON document containing an `errors` array
and a `summary` object. The dump options produce versioned deterministic token, AST, symbol-table, typed-IR, CFG, and
instruction-level source-map artifacts; see [DUMP_FORMATS.md](DUMP_FORMATS.md).

The compiler runs IR peephole and dataflow optimizations before code generation. `--dump-ir-before-opt` captures the
lowered input to those passes, while `--dump-ir` and `--dump-cfg` show their selected output. Use `-O0` to disable the
passes entirely. See
[IR_OPTIMIZATION.md](IR_OPTIMIZATION.md) for transformations and semantic limits.

`--ide --dump-ast <path>` performs editor analysis without code generation or changing assembly output. It recovers from
syntax errors where possible and dumps a validated partial AST with semantic information, even when it exits with
errors. Malformed statements/declarations are omitted; lexer failures can prevent a dump.
`--ide-buffer <path>` reads the root document from an editor snapshot while retaining the original source path for
imports, AST identities, and diagnostic source context. Imported dependencies are still read from disk. This option
requires `--ide`. This mode cannot be combined with assembly, IR, source-map or token output options.

Source diagnostics include the file location, source line, and an underline. Missing semicolons point to the end of the
preceding statement and include an insertion suggestion, even when the next token is on a later nonempty line. Parser
and semantic diagnostics also provide `endLine`/`endColumn` in JSON; coordinates are one-based, the end is exclusive,
and zero denotes an unavailable location. Columns count Unicode code points, with a tab counting as one column.
`sourceLine` preserves UTF-8 text and tabs and excludes line terminators. If the source cannot be read, the diagnostic
still reports its known location.

Type errors use `T` codes; semantic errors use `S`, parser errors `P`, filesystem code-generation failures `G`, and
driver/import failures `C`. Older versions emitted several unrelated semantic and type errors with the same `S` code.
See
[DIAGNOSTICS_AUDIT.md](DIAGNOSTICS_AUDIT.md) for the current inventory and audit results. Related locations are included
in `children`. Human output displays up to ten errors and reports omissions; JSON keeps all errors and includes
`summary.suppressedErrorCount` (zero in compiler JSON mode).

Safe corrections are shown as `help:` text and, when available, a JSON `fix`
object containing `filename`, `line`, `column`, `endLine`, `endColumn`, and
`replacement`. An empty span means insert at that position; `fix: null` means there is no safe automatic edit. Edits are
suggestions and are never applied by the compiler. Unsupported method references such as `node.Next` produce
`S111`; an unambiguous zero-argument method with a valid receiver and a non-void return can suggest inserting `()` after
the method name. Overloads, required arguments, void returns, or invalid receivers suppress the edit.

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
- `tests/fuzz/`: libFuzzer entry points and seed corpora for the lexer, parser, semantic analysis, IR lowering/verifier,
  and syntax converter.

Every normal DMM execution test is compiled, assembled, and run in both Intel and AT&T syntax. Both runs must have exit
code zero, empty stderr, and output identical to the checked-in `.expected` file. This makes syntax equivalence a
standard property of every execution test rather than a separate smoke test.

Test labels can select a subset, for example `ctest --test-dir build -L unit`. On Linux, `-DDMM_SANITIZERS=ON`
instruments the compiler and unit tests and also links generated execution/ABI programs against ASan/UBSan runtimes. CI
runs the
`safety` label with leak detection enabled.

Clang users can build all fuzzers with:

```sh
cmake -S . -B fuzz-build -DDMM_BUILD_FUZZERS=ON -DCMAKE_C_COMPILER=clang
cmake --build fuzz-build --target fuzz_lexer fuzz_parser fuzz_semantic fuzz_ir fuzz_syntax_converter
```

See [FUZZING.md](FUZZING.md) for corpus commands and harness behavior.

## Documentation

- [Examples](examples/README.md): standalone demos of language features and input/output.
- [Formal language](FORMAL_LANGUAGE.md): lexical and syntactic EBNF.
- [Language specification](LANGUAGE_SPEC.md): semantic rules and implementation limits.
- [Architecture](ARCHITECTURE.md): compiler pipeline, module ownership, backend, and validation design.
- [Core runtime](CORE_RUNTIME.md): low-level compiler primitives and DMM library responsibilities.
- [Fuzzing](FUZZING.md): libFuzzer builds, corpora, and target behavior.
- [Dump formats](DUMP_FORMATS.md): stable AST, IR, and machine-instruction source maps.

Source code is grouped by ownership under `src/common`, `src/ast`,
`src/frontend`, `src/sema`, `src/ir`, `src/backend`,
`src/diagnostics`, and `src/driver`.

## License

MIT. See [LICENSE](LICENSE).
