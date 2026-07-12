# Compiler diagnostics

Reviewed on 2026-09-27 against the current diagnostic definitions, reporting sites and test corpus.

This document describes the diagnostic contract and its validation. The numeric constants in
[`src/diagnostics/errorHandler.h`](src/diagnostics/errorHandler.h) are the authoritative code inventory; duplicating
their line numbers and active-call-site list here made the previous audit stale whenever code moved.

## Diagnostic families

| Prefix | Category | Examples |
|---|---|---|
| `L` | lexer | malformed literals, escapes, UTF-8 and source reads |
| `P` | parser | unexpected or missing tokens and invalid declarations |
| `T` | type system | unknown types, invalid operations and incompatible values |
| `S` | semantic analysis | resolution, control flow, ownership, borrows, packages and modules |
| `G` | code generation | output and target-emission failures |
| `C` | compiler driver | entry point, options, imports, dumps and internal failures |
| `W` | warnings | reserved warning IDs; warning policy is still roadmap work |

Numeric values are stable identifiers within a category, not a global sequence. A code must be paired with its category
prefix. Package and module diagnostics occupy the semantic `S120`–`S130` range.

## Rendering contract

Human diagnostics contain the category/code, message, source path and, when available, a source line with a one-based
start and end-exclusive underline. Tabs count as one source column; Unicode columns count code points. Diagnostics with
no source span still identify the relevant path or compiler operation.

`--formatError` emits one JSON document with an `errors` array and a `summary` object. Each diagnostic can contain:

- `code`, `category`, `severity` and `message`
- `filename`, `line`, `column`, `endLine` and `endColumn`
- `sourceLine`
- related diagnostics in `children`
- either a machine-applicable `fix` or `null`

Coordinates are one-based and end-exclusive; zero means unavailable. Invalid source bytes are replaced safely during
JSON serialization. Human output displays at most ten errors and reports omissions, while JSON retains the complete
buffer and records `summary.suppressedErrorCount`.

Fixes are suggestions and are never applied by the compiler. They are emitted only when the replacement and source span
are unambiguous. Missing semicolons can suggest insertion; a method reference can suggest `()` only for a valid,
unambiguous zero-argument non-void method.

## Diagnostic quality rules

- Report the source cause once; suppress dependent conversion or backend noise.
- Underline the supplied value for initializer, assignment, argument, return and enum-payload conversion failures.
- Attach the previous declaration or opening delimiter as a related location when useful.
- Preserve the real failing source, import, output, dump or source-map path and include the operating-system reason.
- Distinguish ownership failures such as use after move, partial moves, loop-carried consumption, escaping borrows,
  package-owner moves and move-only interface erasure.
- Treat internal lowering, verification and emission failures as compiler diagnostics with function, instruction, type
  and source-span context rather than assertions visible to users.

## Validation

The current corpus contains 157 rejection programs and 11 runtime-failure programs. The following CTest targets cover
the diagnostic surface:

- `diagnostics_audit` runs every rejection fixture in human and JSON modes, checks counts and validates source spans.
- `diagnostics_cases` checks exact codes, ranges, messages, related locations, output paths and error limits.
- `diagnostics_json` covers fixes, ambiguity suppression, imports, Unicode, CRLF, EOF and multiple buffered errors.
- `lexer_unit` covers invalid UTF-8 serialization, Unicode columns, literal recovery and escaping.
- `resource_fault_unit` injects allocation and file-operation failures through the frontend, semantic model, IR,
  native object emission and output cleanup paths.
- parser, semantic and IR fuzz targets exercise malformed inputs without bypassing the normal diagnostic handler.

Run the focused suite with:

```sh
ctest --test-dir build -R "diagnostics_(audit|cases|json)" --output-on-failure
```

The audit demonstrates coverage of the checked corpus and injected failures; it is not a proof that every hostile input
or operating-system failure produces an ideal diagnostic.
