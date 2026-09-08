he compiler’s next major addition should be a real typed frontend. The directory split exists, but the semantic split is currently incomplete.

The current suite passes locally: 13/13 tests.

## Highest-priority additions

Priority    Addition                                        Why it is needed                                                                                                                                                                                                                                                                                                                                                  
━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
P0          Full parsed AST                                 The current “AST” primarily stores copied tokens and top-level token ranges.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P0          Separate semantic-analysis pass                 Parsing, type checking, symbol resolution, and assembly generation are still interleaved in the backend.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P0          Typed intermediate representation               Code generation needs a target-neutral input so stack behavior and ABI rules are explicit and testable.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P1          Structured x86-64 instruction representation    Generating AT&T text and converting it to Intel syntax is fragile and duplicates responsibilities.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P1          Dynamic compiler storage                        Fixed symbol tables, literal arrays, and 1 MiB text buffers impose artificial limits.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P1          Centralized target/ABI model                    System V and Windows conventions should be data owned by the backend context, without global target state.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P1          Parser/sema/backend fuzzing                     Current fuzzing covers only the lexer and syntax converter.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P2          Runtime library abstraction                     I/O, strings, heap functions, bounds failures, and platform differences should not be scattered through expression parsing.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P2          Better developer output                         AST dumps, IR dumps, source maps, debug information, dependency output, and direct assemble/link modes.
──────────  ──────────────────────────────────────────────  ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
P3          Additional language features                    Constants, global data, richer arrays, interfaces, generics, and similar features should wait until the middle-end is stable.

## 1. Replace the token-container AST

The current AST declaration contains only a kind, span, and token range in src/ast/ast.h:36. The backend then converts those AST tokens back into a TokenStream in src/backend/backend.c:13 and calls parse_program again at src/backend/backend.c:167.

That means the effective pipeline is still:

lexer → token-owning AST → reconstructed tokens → backend parser/type checker/code generator

The required pipeline should be:

lexer → parser → syntax AST → semantic analysis → typed AST/IR → backend

Useful AST node families include:

- AstDecl: import, function, struct, enum
- AstStmt: block, declaration, assignment, conditional, loops, return, calls
- AstExpr: literal, name, unary, binary, call, index, member, cast
- AstTypeRef: primitive, named, pointer, array
- Parameter, field, and enum-value nodes
- Source spans on every node
- Arena-based ownership for efficient cleanup

After this change, no backend file should include lexer or parser interfaces.

## 2. Add a semantic-analysis layer

A new src/sema/ or src/frontend/sema/ layer should perform:

- Symbol declaration and resolution
- Lexical scope construction
- Type resolution
- Implicit and explicit conversion checking
- Struct and array layout
- Function signature validation
- Return-path analysis
- Lvalue validation
- Constant expression evaluation
- Import graph validation
- Assignment of resolved symbol and type IDs to AST nodes

The backend should receive expressions whose types and referenced symbols are already known. It should never report errors such as “undefined variable” or decide whether a conversion is legal while emitting instructions.

The current combined lowerer is already acknowledged as the remaining constraint in ARCHITECTURE.md:117.

## 3. Introduce a typed IR

A small IR would materially improve reliability. It does not need to resemble LLVM; a basic block and three-address representation is sufficient.

For example:

%1 = call mark(1) : int
%2 = call mark(2) : int
%3 = add %1, %2 : int
call println_int(%3)
return

This would provide:

- Explicit evaluation order
- Known value types and widths
- Basic blocks and control-flow edges
- Calls with typed argument lists
- Stack-frame calculation before emission
- Easier constant folding and dead-code elimination
- A clean place for bounds and null checks
- Unit testing without assembling and executing a program

The recent nested-call stack-alignment bug is exactly the kind of defect an IR and explicit call-frame model would help prevent.

## 4. Replace text-based instruction conversion

The backend currently emits assembly text and uses a syntax converter. A better approach is:

IR → x86-64 instruction objects → AT&T printer
└──→ Intel printer

An instruction object should know:

- Opcode
- Operand width
- Registers
- Immediate operands
- Memory operands
- Labels
- Relocations
- Comments

This removes string parsing from the compiler’s core correctness path. Eventually, it also makes direct object-file emission possible.

## 5. Remove fixed-capacity compiler state

src/backend/compiler_types.h:132 contains fixed arrays for variables, functions, structs, enums, and literals, plus two fixed assembly buffers at src/backend/compiler_types.h:139.

These should become checked dynamic containers:

- Symbol vectors or hash tables
- Interned identifier/string table
- Dynamic diagnostics
- Growable instruction/function lists
- Per-function lowering contexts
- Dynamic import set
- Arena allocation for AST and IR

This will reduce memory usage too. Every token currently reserves 512 bytes for its lexeme, and the backend creates a second token copy. Using source offsets or interned strings would make large files substantially cheaper.

## 6. Strengthen type representation

Types are currently represented through DataType plus independent flags and struct-name strings. This becomes difficult to maintain for combinations such as pointers to structs, arrays of pointers, or future composite types.

Add an interned type system such as:

TypeId
primitive(int)
pointer(element_type)
array(element_type, length)
structure(symbol_id)
function(parameters, result)

Literal typing also needs to be explicit. The assembly writer currently selects float versus double storage based on the number of characters after the decimal point in src/backend/backend.c:115. Literal width should instead come from semantic type information.

## 7. Centralize targets and runtime operations

Target state is partly stored in Parser and partly set globally through set_target_format at src/backend/backend.c:165.

Introduce an immutable backend target description containing:

- Object format
- Calling convention
- Argument and return registers
- Nonvolatile registers
- Stack alignment and shadow space
- Symbol naming
- Section directives
- Variadic-call rules
- Supported runtime/system operations

A src/runtime/ layer should define string, input/output, allocation, GC, and trap operations. Direct handling of strcpy, strcat, malloc, scanf, and system calls inside expression parsing should disappear.

## 8. Expand correctness testing

The existing testing setup is a strong base, especially its dual-syntax execution, ABI tests, sanitizers, rejection tests, and limit tests. The largest gaps are:

- AST parser fuzzing
- Semantic analyzer fuzzing
- IR verifier tests
- Backend instruction/printer fuzzing
- Nested calls with every combination of integer, floating, and stack arguments
- Aggregate arguments and returns
- Very large function argument lists
- Import cycles and deep import graphs
- Diagnostic recovery producing multiple errors
- Null dereference and division-by-zero behavior
- String capacity and overlapping-copy behavior
- Randomized differential testing between unoptimized and optimized output

Only the lexer and syntax converter currently have fuzz targets in CMakeLists.txt:75.

## Recommended implementation order

1. Define complete AST node types and arena ownership.
2. Move parsing entirely into the frontend.
3. Add symbol tables and semantic analysis.
4. Make the backend consume typed AST nodes directly.
5. Add a small typed IR and IR verifier.
6. Split the backend into lowering, x86_64, abi, and assembly-printer directories.
7. Replace fixed compiler buffers with dynamic collections.
8. Add parser, semantic, and backend fuzzers.
9. Extract runtime operations.
10. Only then expand the language.