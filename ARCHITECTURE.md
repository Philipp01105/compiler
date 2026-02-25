# DMM Compiler Architecture

## Overview

DMM is a single-process C23 compiler that translates one DMM source file and its imports into GNU x86-64 assembly. It supports ELF/System V and COFF/Windows targets and can emit AT&T or Intel syntax.

```text
source and imports
       |
       v
frontend: lexer -> owned AST (declarations, lossless leaves, source spans)
       |
       | AstProgram
       v
backend: semantic lowering -> x86-64 emission -> syntax conversion
       |
       v
conservative peephole pass -> assembly file
```

`AstProgram` is the only public value crossing the frontend/backend boundary.
It owns its data, so lexer storage is released before the backend runs. The
boundary is checked by `architecture_boundaries`: the CLI cannot include lexer
or parser APIs, the frontend cannot depend on backend implementation headers,
and public AST/backend headers cannot expose legacy parser state.

## Entry point and compilation lifecycle

`src/driver/main.c` owns the command-line interface and compilation lifecycle:

1. Parse output, target, syntax, diagnostics, and debugging options.
2. Initialize the diagnostic handler.
3. Ask the frontend to tokenize and build an owned `AstProgram`.
4. Release all lexer-owned state.
5. Pass the AST and immutable target options to `backend_emit_file`.
6. Lower declarations, write data sections, literals, and function code.
7. Run the conservative assembly cleanup pass.
8. Release parser, token, and diagnostic state.

Failed compilation removes the default stale assembly output. Explicit output paths are protected from overwriting the source file.

## Front end

`src/frontend/lexer.c` converts files or in-memory byte sequences into the token types declared in `src/common/language_types.h`. Tokens retain line and column positions. `src/frontend/frontend.c` copies them into lossless AST leaves, constructs top-level declaration nodes, and destroys the lexer stream before returning. Lexical failures use the common diagnostic handler and can therefore be emitted as human-readable text or one JSON document. The in-memory entry point is also the boundary used by the lexer fuzzer.

The parser is divided by responsibility:

- `backend/parser_core.c`: lowering lifecycle, diagnostics, imports, symbol lookup, and output-buffer helpers.
- `backend/parser_declarations.c`: forward function signatures, functions, structs, enums, parameters, and frame layout.
- `backend/parser_statements.c`: blocks, variables, assignments, control flow, printing, I/O, heap operations, and statement-level calls.
- `backend/parser_expressions.c`: precedence parsing, literals, calls, indexing, member access, casts, and expression emission.
- `backend/parser_types.c`: type names, sizes, target calling-convention tables, and type-related helpers.

Forward signatures allow calls to functions declared later in the source. Imports are resolved relative to the current file and recorded in the parser context to prevent duplicate processing.

## Semantic state

`Parser` in `backend/compiler_types.h` is the private compatibility-lowering context. It owns token position, symbol tables, literal tables, scopes, loop state, source context, target/syntax selection, and generated data/function buffers.

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

## AST boundary

`src/ast/ast.h` contains only language-level types. An `AstProgram` owns its source
identity, token leaves, source spans, and declaration nodes. It has no parser,
lexer-stream, output-buffer, target, or assembly state. `src/frontend/frontend.h` creates
it and `src/backend/backend.h` consumes it; neither public API includes the other
layer's implementation headers.

## Back end

The backend entry point accepts an immutable `AstProgram` and immutable target
options. Compatibility lowering for the full existing language remains private
inside `backend.c`; no legacy parser state crosses back into the CLI or
frontend. It directly emits x86-64 instructions into checked lowering buffers.

- `backend/parser_codegen.c` contains conversion, ABI argument lowering, stack-call helpers, bounds checks, and low-level runtime calls.
- `backend/instruction_builder.c` provides focused instruction emitters. Syntax selection comes from the active parser context rather than global syntax state.
- `backend/syntax_converter.c` converts bounded AT&T-form instruction lines to GNU Intel syntax when requested.
- `backend/asm_optimizer.c` performs only semantics-preserving adjacent push/pop removal. It streams the input and replaces it through a unique same-directory temporary file, preserving the original on failure.

Target selection controls argument registers, stack shadow space, section directives, exported symbol metadata, and supported I/O lowering. Generated assembly is assembled and linked by an external GNU-compatible toolchain.

## Diagnostics

`diagnostics/errorHandler.c` owns diagnostic contexts, severity, categories, source locations, buffering, and rendering. Normal mode prints contextual text. `--formatError` prints one JSON object with an `errors` array and summary counts. Lexer, parser, semantic, code-generation, option, allocation, and file failures use this path.

## Limits and safety boundaries

The major limits are defined in `compiler_types.h`, including token size, symbol counts, literal counts, generated-code capacity, import count, loop depth, expression complexity, and maximum object/frame storage. Size arithmetic and dynamic allocation paths are checked before use.

Generated instructions have a bounded intermediate representation. The syntax converter rejects input or expansion that cannot fit rather than silently truncating it. Assembly optimization has no fixed line-count limit.

## Build and validation architecture

The root `CMakeLists.txt` builds the compiler, exposes strict-warning and sanitizer options, and registers labeled tests. CTest separates native execution, ABI, rejection, backend safety, optimizer, converter, JSON diagnostics, CLI behavior, and imports. The execution corpus compiles, assembles, and runs programs in both assembly syntaxes.

GitHub Actions validates Linux, Linux with AddressSanitizer/UndefinedBehaviorSanitizer, and Windows with MinGW-w64. Clang libFuzzer targets are available for the lexer and syntax converter through `DMM_BUILD_FUZZERS`.

## Remaining internal constraint

The public compiler pipeline is split at an owned AST, but the compatibility
lowerer behind the backend API still combines detailed statement parsing,
semantic checks, and instruction selection. New code must preserve the public
AST-only boundary. The next internal refinement is to replace lossless statement
leaves with typed statement/expression nodes and make backend lowering a pure
visitor; this can happen without changing the CLI or backend API.
