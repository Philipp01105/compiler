# Compiler diagnostic audit

Reviewed on 2026-09-13.

All defined error and warning constants and reporting sites in the lexer, parser,
semantic analyzer, driver, backend and shared renderer were reviewed. Runtime
checks cover all 77 rejection fixtures in both human-readable and JSON modes,
plus focused location, message, filesystem and encoding cases.

## Corrections

- Type errors now use `T` codes. Semantic declarations, complexity, storage and
  bounds errors have distinct `S` codes; missing `main` uses `C100`. Previously
  unrelated errors collided because semantic reporting always used `S`.
- Expression syntax errors are reported before advancing beyond the bad token.
  Expected-token messages include the actual token or end of file. Names and
  keywords have meaningful spellings instead of the generic word `token`.
- Missing braces/parentheses include a note at the opening delimiter. Duplicate
  declarations include the previous declaration, including imported files.
- Conversion errors show supplied and expected types and underline the value
  expression for initializers, assignments, arguments, returns and enum arguments.
  An unresolved value does not produce an additional conversion error.
- Invalid escapes mark the backslash and give supported escapes. Character
  literals with several bytes are distinguished from unterminated literals.
  Strings containing raw line terminators are rejected as required by the grammar;
  escaped single quotes in strings are accepted.
- Multiple decimal points are distinguished from oversized numeric tokens.
  Exponent parsing respects the token buffer boundary.
- Unknown Unicode characters count as one source column. Invalid UTF-8 bytes
  are identified without placing invalid bytes in JSON; invalid text in JSON
  fields is replaced with escaped U+FFFD.
- Filesystem failures identify the actual source, import, assembly, dump or
  source-map path. Open/write failures include the operating-system reason.
  Backend failures are reported once; internal emission failures are distinguished
  from failure to open the output.
- Human output identifies paths even for errors without a source location.
  Human output limits errors to ten and explicitly reports omitted errors;
  JSON retains every error. Resetting a handler clears buffered diagnostics.

## Code inventory

`Active` means referenced by current compiler implementation, not that every
branch was forced in a runtime test. Unused constants are retained for compatibility;
they do not imply an implemented diagnostic or warning.

| Code | Constant | Status | Implementation |
| --- | --- | --- | --- |
| `L100` | `ERR_LEX_UNCLOSED_STRING` | Active | [lexer.c:333](src/frontend/lexer.c#L333) |
| `L101` | `ERR_LEX_UNCLOSED_CHAR` | Active | [lexer.c:413](src/frontend/lexer.c#L413) |
| `L102` | `ERR_LEX_INVALID_ESCAPE` | Active | [lexer.c:286](src/frontend/lexer.c#L286) |
| `L103` | `ERR_LEX_UNKNOWN_CHAR` | Active | [lexer.c:600](src/frontend/lexer.c#L600) |
| `L104` | `ERR_LEX_FILE_NOT_FOUND` | Active | [lexer.c:178](src/frontend/lexer.c#L178) |
| `L105` | `ERR_LEX_FILE_READ_ERROR` | Active | [lexer.c:59](src/frontend/lexer.c#L59) |
| `L106` | `ERR_LEX_INVALID_SYNTAX` | Active | [lexer.c:453](src/frontend/lexer.c#L453) |
| `L107` | `ERR_LEX_INVALID_CHAR_LITERAL` | Active | [lexer.c:387](src/frontend/lexer.c#L387) |
| `L108` | `ERR_LEX_TOKEN_TOO_LONG` | Active | [lexer.c:317](src/frontend/lexer.c#L317) |
| `P100` | `ERR_PARSE_UNEXPECTED_TOKEN` | Active | [syntax_parser.c:281](src/frontend/syntax_parser.c#L281) |
| `P101` | `ERR_PARSE_EXPECTED_TOKEN` | Active | [syntax_parser.c:132](src/frontend/syntax_parser.c#L132) |
| `P102` | `ERR_PARSE_INVALID_SYNTAX` | Active | [main.c:310](src/driver/main.c#L310) |
| `P103` | `ERR_PARSE_MISSING_SEMICOLON` | Active | [syntax_parser.c:116](src/frontend/syntax_parser.c#L116) |
| `P104` | `ERR_PARSE_MISSING_BRACE` | Active | [syntax_parser.c:37](src/frontend/syntax_parser.c#L37) |
| `P105` | `ERR_PARSE_MISSING_PAREN` | Active | [syntax_parser.c:37](src/frontend/syntax_parser.c#L37) |
| `P106` | `ERR_PARSE_INVALID_DECLARATION` | Active | [syntax_parser.c:862](src/frontend/syntax_parser.c#L862) |
| `P107` | `ERR_PARSE_DUPLICATE_DEFINITION` | Unused |  |
| `P108` | `ERR_PARSE_TOO_MANY_ERRORS` | Active | [syntax_parser.c:365](src/frontend/syntax_parser.c#L365) |
| `T100` | `ERR_TYPE_MISMATCH` | Unused |  |
| `T101` | `ERR_TYPE_UNKNOWN` | Active | [semantic.c:51](src/sema/semantic.c#L51) |
| `T102` | `ERR_TYPE_INVALID_OPERATION` | Active | [semantic.c:1125](src/sema/semantic.c#L1125) |
| `T103` | `ERR_TYPE_INCOMPATIBLE_TYPES` | Active | [semantic.c:998](src/sema/semantic.c#L998) |
| `S100` | `ERR_SEM_UNDEFINED_VARIABLE` | Active | [semantic.c:53](src/sema/semantic.c#L53) |
| `S101` | `ERR_SEM_UNDEFINED_FUNCTION` | Active | [semantic.c:1371](src/sema/semantic.c#L1371) |
| `S102` | `ERR_SEM_UNDEFINED_STRUCT` | Unused |  |
| `S103` | `ERR_SEM_WRONG_ARG_COUNT` | Active | [semantic.c:1292](src/sema/semantic.c#L1292) |
| `S104` | `ERR_SEM_NOT_ARRAY` | Active | [semantic.c:1542](src/sema/semantic.c#L1542) |
| `S105` | `ERR_SEM_NOT_STRUCT` | Active | [semantic.c:1601](src/sema/semantic.c#L1601) |
| `S106` | `ERR_SEM_FIELD_NOT_FOUND` | Active | [semantic.c:54](src/sema/semantic.c#L54) |
| `S107` | `ERR_SEM_METHOD_NOT_FOUND` | Unused |  |
| `S108` | `ERR_SEM_NOT_STATIC` | Unused |  |
| `S109` | `ERR_SEM_BREAK_OUTSIDE_LOOP` | Active | [semantic.c:2264](src/sema/semantic.c#L2264) |
| `S110` | `ERR_SEM_CONTINUE_OUTSIDE_LOOP` | Active | [semantic.c:2267](src/sema/semantic.c#L2267) |
| `S111` | `ERR_SEM_METHOD_REFERENCE` | Active | [semantic.c:1320](src/sema/semantic.c#L1320) |
| `S112` | `ERR_SEM_DUPLICATE_DEFINITION` | Active | [semantic.c:53](src/sema/semantic.c#L53) |
| `S113` | `ERR_SEM_INVALID_DECLARATION` | Active | [semantic.c:248](src/sema/semantic.c#L248) |
| `S114` | `ERR_SEM_COMPLEXITY_LIMIT` | Active | [semantic.c:1310](src/sema/semantic.c#L1310) |
| `S115` | `ERR_SEM_STORAGE_LIMIT` | Active | [semantic.c:2173](src/sema/semantic.c#L2173) |
| `S116` | `ERR_SEM_INDEX_OUT_OF_BOUNDS` | Active | [semantic.c:1594](src/sema/semantic.c#L1594) |
| `G100` | `ERR_CODEGEN_TOO_MANY_LITERALS` | Unused |  |
| `G101` | `ERR_CODEGEN_TOO_MANY_VARIABLES` | Unused |  |
| `G102` | `ERR_CODEGEN_TOO_MANY_FUNCTIONS` | Unused |  |
| `G103` | `ERR_CODEGEN_OUTPUT_FAILED` | Active | [backend.c:12](src/backend/backend.c#L12), [ir_emitter.c:2324](src/backend/x86_64/ir_emitter.c#L2324) |
| `C100` | `ERR_COMP_NO_MAIN_FUNCTION` | Active | [semantic.c:2577](src/sema/semantic.c#L2577) |
| `C101` | `ERR_COMP_NO_SOURCE_FILE` | Active | [main.c:257](src/driver/main.c#L257) |
| `C102` | `ERR_COMP_INVALID_OPTION` | Active | [main.c:166](src/driver/main.c#L166) |
| `C103` | `ERR_COMP_INTERNAL_FAILURE` | Active | [main.c:303](src/driver/main.c#L303), [frontend.c:144](src/frontend/frontend.c#L144), [syntax_parser.c:152](src/frontend/syntax_parser.c#L152) |
| `C104` | `ERR_COMP_IMPORT_NOT_FOUND` | Active | [frontend.c:356](src/frontend/frontend.c#L356) |
| `C105` | `ERR_COMP_IMPORT_OUTSIDE_ROOT` | Active | [frontend.c:389](src/frontend/frontend.c#L389) |
| `C106` | `ERR_COMP_DUMP_FAILED` | Active | [main.c:306](src/driver/main.c#L306) |
| `W100` | `WARN_UNUSED_VARIABLE` | Unused |  |
| `W101` | `WARN_DEPRECATED` | Unused |  |

## Validation and limits

- `diagnostics_audit`: all 77 existing rejection fixtures fail as expected,
  produce parseable JSON with consistent counts, and have matching human
  messages, locations and source lines. Source spans are checked for validity.
- `diagnostics_cases`: exact codes, columns and messages for targeted failures,
  conversion ranges, declaration/opening notes, output paths and error limits.
- `diagnostics_json`: method-reference fixes, ambiguity suppression, imports,
  Unicode/CRLF, EOF and multiple errors in one JSON document.
- `lexer_unit`: invalid UTF-8 serialization, Unicode columns, character recovery
  and escaped quotes. Existing frontend, execution, fuzz and safety tests remain.
- Plugin `check` is run with the rebuilt compiler to verify JSON consumption and
  type hover, including syntax recovery.

`resource_fault_unit` deterministically fails every nonzero allocation in a generic
frontend/semantic/IR pipeline and native ELF/COFF emission, and every wrapped file
open/write/close operation. Failed output writes remove partial artifacts. Zero-byte
allocations are excluded because a NULL result is permitted. The sweep runs under
ASan/UBSan and covers cleanup after partially initialized enum metadata.
Internal IR/emitter diagnostics include the function, instruction, operands, type
and source span. Native filesystem failures retain the failing path, operation and
system error. Operator diagnostics print supplied concrete types and complete shapes.
Injected I/O failures model permission denial, disk exhaustion and close failure;
they do not simulate every filesystem or operating-system failure.
Machine/source columns count Unicode code points; terminal alignment of wide glyphs
depends on the terminal/font. This audit is not proof for every possible input.
