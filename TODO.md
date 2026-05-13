# Compiler roadmap

The P0 frontend and typed-IR migration is complete. The production pipeline is:

```text
source -> lexer -> structured AST -> semantic analysis -> verified typed IR
       -> x86-64 emitter -> assembly cleanup
```

There is one code-generation path. The compatibility backend and its combined
parser/type-checker/emitter have been removed from the build and source tree.

## Completed P0 work

- Full structured AST for imports, functions, structs, enums, types,
  statements, and precedence-aware expressions, with source spans.
- Separate semantic pass for symbols, scopes, named types, conversions,
  lvalues, calls, returns, member access, loop control, and constant bounds.
- Target-neutral typed IR with explicit values, calls, aggregate access,
  allocation, cleanup, labels, branches, jumps, and short-circuit PHI nodes.
- Verified IR as the sole x86-64 backend input.
- System V and Windows x64 call lowering, including mixed integer/SSE and stack
  arguments, imported functions, instance-method receivers, and mangled methods.
- Native lowering for numeric types, strings, arrays, structs, enums, pointers,
  heap allocation, `@gc`, printing, input, filesystem intrinsics, and stdlib I/O.
- Nominal aggregate checking and by-value struct initialization, assignment,
  parameters, nested storage, and returns; first-class enum values and constant
  scalar payload lookup.
- Typed verifier coverage for every opcode, including operations, calls, returns,
  aggregate access, control flow, and malformed-definition mutation tests.
- Collision-safe method/runtime symbol mangling while preserving ordinary
  top-level names for C ABI interoperability.
- Dual Intel/AT&T execution tests plus independent ABI, diagnostics, import,
  runtime-failure, stress, and IR-native tests.
- Structured x86-64 instructions and typed operands with data-driven Intel and
  AT&T printers; the IR emitter no longer formats machine instructions directly.
- Module-wide interned token spellings shared across imports, plus a hashed
  semantic symbol index for fast identifier lookup.
- Dedicated libFuzzer targets and seed corpora for parser, semantic analysis,
  IR lowering, and malformed-IR verification, with a portable CTest smoke path.
- Versioned deterministic AST and typed-IR dumps plus per-x86-instruction source
  maps that retain source unit, function, IR index, and exact source span.

## Next priorities

| Priority | Addition                                | Why                                                                                                       |
|----------|-----------------------------------------|-----------------------------------------------------------------------------------------------------------|
| P2       | Runtime library split                   | Move platform runtime shims out of the emitter while retaining typed runtime operations in IR.            |
| P2       | Direct object emission                  | Avoid the external assembler when the instruction model is mature.                                        |
| P3       | New language features                   | Consider constants, globals, richer arrays, interfaces, and generics after the middle-end remains stable. |
