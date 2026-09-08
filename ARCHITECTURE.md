# DMM Compiler Architecture

## Overview

DMM is a single-process C23 compiler that translates one DMM source file and its imports into GNU x86-64 assembly. It supports ELF/System V and COFF/Windows targets and can emit AT&T or Intel syntax.

```text
source and imports
       |
       v
lexer -> token streams -> parser / semantic checks -> x86-64 emission
                                                     |
                                                     v
                                           syntax conversion
                                                     |
                                                     v
                                      conservative peephole pass
                                                     |
                                                     v
                                               assembly file
```

The current implementation emits assembly while parsing; it does not build an AST or intermediate representation.

## Entry point and compilation lifecycle

`src/main.c` owns the command-line interface and compilation lifecycle:

1. Parse output, target, syntax, diagnostics, and debugging options.
2. Initialize the diagnostic handler.
3. Tokenize the root source file.
4. Create and configure a `Parser` context.
5. Parse declarations and emit assembly into parser-owned buffers.
6. Write data sections, literals, and function code to the requested output.
7. Run the conservative assembly cleanup pass.
8. Release parser, token, and diagnostic state.

Failed compilation removes the default stale assembly output. Explicit output paths are protected from overwriting the source file.

## Front end

`src/lexer.c` converts source bytes into the token types declared in `src/compiler_types.h`. Tokens retain line and column positions. Lexical failures use the common diagnostic handler and can therefore be emitted as human-readable text or one JSON document.

The parser is divided by responsibility:

- `parser_core.c`: parser lifecycle, diagnostics, imports, symbol lookup, and output-buffer helpers.
- `parser_declarations.c`: forward function signatures, functions, structs, enums, parameters, and frame layout.
- `parser_statements.c`: blocks, variables, assignments, control flow, printing, I/O, heap operations, and statement-level calls.
- `parser_expressions.c`: precedence parsing, literals, calls, indexing, member access, casts, and expression emission.
- `parser_types.c`: type names, sizes, target calling-convention tables, and type-related helpers.

Forward signatures allow calls to functions declared later in the source. Imports are resolved relative to the current file and recorded in the parser context to prevent duplicate processing.

## Semantic state

`Parser` in `compiler_types.h` is the central compilation context. It owns token position, symbol tables, literal tables, scopes, loop state, source context, target/syntax selection, and generated data/function buffers.

Variables and functions store their resolved types and ABI-relevant flags. Semantic checks occur before or alongside emission, including:

- duplicate and undefined symbols;
- implicit-conversion compatibility;
- function argument count and types;
- return type and all-path return validation;
- loop-control placement;
- assignable lvalues;
- static object and aggregate frame sizes;
- compile-time literal array bounds.

Dynamic accesses to fixed local arrays emit runtime lower- and upper-bound checks and trap on failure.

## Back end

The backend directly emits x86-64 instructions into checked parser buffers.

- `parser_codegen.c` contains conversion, ABI argument lowering, stack-call helpers, bounds checks, and low-level runtime calls.
- `instruction_builder.c` provides focused instruction emitters. Syntax selection comes from the active parser context rather than global syntax state.
- `syntax_converter.c` converts bounded AT&T-form instruction lines to GNU Intel syntax when requested.
- `asm_optimizer.c` performs only semantics-preserving adjacent push/pop removal. It streams the input and replaces it through a unique same-directory temporary file, preserving the original on failure.

Target selection controls argument registers, stack shadow space, section directives, exported symbol metadata, and supported I/O lowering. Generated assembly is assembled and linked by an external GNU-compatible toolchain.

## Diagnostics

`errorHandler.c` owns diagnostic contexts, severity, categories, source locations, buffering, and rendering. Normal mode prints contextual text. `--formatError` prints one JSON object with an `errors` array and summary counts. Lexer, parser, semantic, code-generation, option, allocation, and file failures use this path.

## Limits and safety boundaries

The major limits are defined in `compiler_types.h`, including token size, symbol counts, literal counts, generated-code capacity, import count, loop depth, expression complexity, and maximum object/frame storage. Size arithmetic and dynamic allocation paths are checked before use.

Generated instructions have a bounded intermediate representation. The syntax converter rejects input or expansion that cannot fit rather than silently truncating it. Assembly optimization has no fixed line-count limit.

## Build and validation architecture

The root `CMakeLists.txt` builds the compiler, exposes strict-warning and sanitizer options, and registers labeled tests. CTest separates native execution, ABI, rejection, backend safety, optimizer, converter, JSON diagnostics, CLI behavior, and imports. The execution corpus compiles, assembles, and runs programs in both assembly syntaxes.

GitHub Actions validates Linux, Linux with AddressSanitizer/UndefinedBehaviorSanitizer, and Windows with MinGW-w64. A Clang libFuzzer target is available for the syntax converter through `DMM_BUILD_FUZZERS`.

## Known structural constraint

Parsing, semantic analysis, and code generation are currently coupled. The natural future boundary is a typed AST or intermediate representation between the front end and backend. Until then, new behavior should keep target-specific lowering in backend helpers and avoid adding mutable global configuration.
