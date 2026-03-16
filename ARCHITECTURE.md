# DMM Compiler Architecture

## Overview

DMM is a single-process C23 compiler that translates one DMM source file and its imports into GNU x86-64 assembly. It supports ELF/System V and COFF/Windows targets and can emit AT&T or Intel syntax.

```text
source and imports
       |
       v
frontend: lexer -> parser -> owned recursive AST
       |
       v
semantic analysis -> resolved expression types and symbols
       |
       v
typed IR -> verifier
       |
       v
backend: typed-IR x86-64 emission -> AT&T/Intel printer
       or isolated compatibility lowering
       |
       v
conservative peephole pass -> assembly file
```

`AstProgram`, `SemanticModel`, and verified `IrModule` form the target-neutral
side of the frontend/backend boundary. The AST owns its data, so lexer storage is
released before semantic analysis runs. The
boundary is checked by `architecture_boundaries`: the CLI cannot include lexer
or parser APIs, the frontend cannot depend on backend implementation headers,
and public AST/backend headers cannot expose legacy parser state.

## Entry point and compilation lifecycle

`src/driver/main.c` owns the command-line interface and compilation lifecycle:

1. Parse output, target, syntax, diagnostics, and debugging options.
2. Initialize the diagnostic handler.
3. Ask the frontend to tokenize and build an owned recursive `AstProgram`.
4. Release all lexer-owned state.
5. Build an independent semantic model and lower the typed AST to verified IR.
6. Pass only the IR module and immutable target options to `backend_emit_file`.
7. Lower declarations, write data sections, literals, and function code.
8. Run the conservative assembly cleanup pass.
9. Release IR, semantic, AST, compatibility-lowering, and diagnostic state.

Failed compilation removes the default stale assembly output. Explicit output paths are protected from overwriting the source file.

## Front end

`src/frontend/lexer.c` converts files or in-memory byte sequences into the token types declared in `src/common/language_types.h`. Tokens retain line and column positions. `src/frontend/syntax_parser.c` constructs declarations, types, parameters, statements, and precedence-aware expressions in an AST arena. `src/frontend/frontend.c` destroys the lexer stream before returning. Lossless leaves remain temporarily for the compatibility emitter, but new stages consume structured nodes. Lexical failures use the common diagnostic handler and can therefore be emitted as human-readable text or one JSON document. The in-memory entry point is also the boundary used by the lexer fuzzer.

## Semantic analysis and IR

`src/sema/semantic.c` owns target-neutral declaration collection, lexical
symbols, and expression type resolution. Resolved types and stable symbol IDs
are attached to expression nodes, while `SemanticModel` retains global,
parameter, and local symbols with ownership and scope metadata.

`src/ir/ir.c` lowers functions and methods to typed values, explicit calls,
loads/stores, branches, labels, loop edges, returns, and print operations. The
IR verifier checks unique SSA definitions, use-before-definition, call argument
ranges, and unique/defined control-flow labels before a module reaches the
backend. The frontend pipeline unit test runs this sequence over every
execution fixture and example.

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
identity, source spans, recursive declarations/statements/expressions, and its
allocation arena. It has no parser,
lexer-stream, output-buffer, target, or assembly state. `src/frontend/frontend.h` creates
it and `src/backend/backend.h` consumes it; neither public API includes the other
layer's implementation headers.

## Back end

The backend entry point accepts a verified `IrModule` and immutable target
options. `backend/x86_64/ir_emitter.c` directly emits scalar locals, integer
arithmetic and comparisons, assignments, branches, loops, returns, and output
from typed IR in either GNU syntax. It computes the complete frame before
emission and keeps calls aligned for System V and Windows.

Constructs not covered by native instruction selection are routed through the
adapter in `src/compat`; legacy parser state does not enter `src/backend`, the
CLI, or the frontend. This adapter preserves the complete existing language
while native lowering expands.

- `backend/parser_codegen.c` contains conversion, ABI argument lowering, stack-call helpers, bounds checks, and low-level runtime calls.
- `backend/instruction_builder.c` provides focused instruction emitters. Syntax selection comes from the active parser context rather than global syntax state.
- `backend/syntax_converter.c` converts bounded AT&T-form instruction lines to GNU Intel syntax when requested.
- `backend/asm_optimizer.c` performs only semantics-preserving adjacent push/pop removal. It streams the input and replaces it through a unique same-directory temporary file, preserving the original on failure.

Target selection controls argument registers, stack shadow space, section directives, exported symbol metadata, and supported I/O lowering. Generated assembly is assembled and linked by an external GNU-compatible toolchain.

Generated functions preserve every non-volatile register used by the backend
(`rbx` and `r12`-`r15`, plus `rdi`/`rsi` for COFF). System V variadic calls set
`al` to a valid upper bound for vector arguments before calling `printf`. These
ABI properties are inspected by backend tests and exercised through the native
C interoperability suite.

## Diagnostics

`diagnostics/errorHandler.c` owns diagnostic contexts, severity, categories, source locations, buffering, and rendering. Normal mode prints contextual text. `--formatError` prints one JSON object with an `errors` array and summary counts. Lexer, parser, semantic, code-generation, option, allocation, and file failures use this path.

## Limits and safety boundaries

The major limits are defined in `compiler_types.h`, including token size, symbol counts, literal counts, generated-code capacity, import count, loop depth, expression complexity, and maximum object/frame storage. Size arithmetic and dynamic allocation paths are checked before use.

Generated instructions have a bounded intermediate representation. The syntax converter rejects input or expansion that cannot fit rather than silently truncating it. Assembly optimization has no fixed line-count limit.

## Build and validation architecture

The root `CMakeLists.txt` builds the compiler, exposes strict-warning and sanitizer options, and registers labeled tests. CTest separates native execution, ABI, rejection, backend safety, optimizer, converter, JSON diagnostics, CLI behavior, and imports. The execution corpus compiles, assembles, and runs programs in both assembly syntaxes.

GitHub Actions validates Linux, Linux with AddressSanitizer/UndefinedBehaviorSanitizer, and Windows with MinGW-w64. Clang libFuzzer targets are available for the lexer and syntax converter through `DMM_BUILD_FUZZERS`.

## Remaining migration constraint

The P0 pipeline and boundary are established and exercised for the complete
valid corpus. Native IR emission currently covers the scalar/control-flow
subset; calls, floating-point values, strings beyond direct output, aggregates,
pointers, heap operations, and imported code use `src/compat`. New language
analysis belongs in `frontend`/`sema`, never in the adapter. Once native
instruction selection covers those constructs, the lossless token index and
compatibility parser can be removed.
