# Compiler debugging dumps

The compiler exposes deterministic, line-oriented debugging formats. Each begins with a version marker; consumers must
reject unknown versions rather than infer a schema from individual fields.

```sh
compiler -S --dump-tokens program.tokens --dump-ast program.ast --dump-symbols program.symbols \
  --dump-ir-before-opt program.lowered.ir --dump-ir program.ir --dump-cfg program.cfg \
  --source-map program.map -o program.s program.dmm
```

Paths must be distinct from all loaded sources, generated output, and one another. Dump creation is part of compilation:
an I/O or validation failure makes the command fail and removes the incomplete artifact.

Dumps are written as soon as their compiler phase has completed. Consequently, `--dump-tokens` remains available after
a parser failure, and `--dump-ast`/`--dump-symbols` remain available after a semantic failure. IR and CFG dumps require
successful semantic analysis and lowering.

## `dmm-native-link-v1`

`--dump-native-link FILE` describes the output target ABI and runtime profile after
function selection, then lists native imports with logical library IDs and native
symbol names. Explicit overrides add their file path; requested search directories
are listed separately. Unused imports are absent. Quoted values escape quotes,
backslashes and newlines. This inventory accompanies object/assembly output without
starting a linker. Platform output still requires the matching installed runtime
shim and regular C startup, as described in [NATIVE_BACKEND.md](NATIVE_BACKEND.md).

The v5 IR instruction inventory also includes `native-copy`, a byte-exact snapshot
of a native struct before evaluation of later call arguments.

## IDE analysis mode

`--ide --dump-ast FILE` performs editor analysis without code generation. It recovers from syntax errors where possible
and writes a validated partial AST with semantic information; malformed declarations may be omitted, and lexer failures
can prevent a dump. `--ide-buffer FILE` reads the root document from an editor snapshot while retaining the original
source path for imports, identities and diagnostics. Imported packages are still read from disk. IDE mode cannot be
combined with program emission, IR, CFG, source-map or token output.

## `dmm-native-map-v1`

Native `--emit=obj` and `--emit=exe` source maps begin with this marker. Records use the assembly map's instruction,
function, source, IR index and span fields, prefixed with `text-offset N`, the instruction's byte offset in the text
section. Executable offsets cover source-generated code before runtime/startup/import thunks are appended. They are
section offsets, not file offsets or addresses.

## `dmm-tokens-v1`

The token dump inventories the root source and all resolved import units. Every token has its stable unit-local index,
token kind, exact escaped source spelling, and begin/end source position. It is useful for diagnosing keyword
classification, escaped literals, source coordinates, and import-specific lexer behavior without enabling noisy
terminal debugging.

## `dmm-ast-v5`

Version 4 adds `native=1`, `opaque=...`, `abi=...`, optional `library=...` and
`native-name=...` on native declarations. Extern blocks flatten into package
declarations while ABI/library/alias token fields retain their source coordinates.
Native structs never carry a library field. Native imports have no body.

The AST dump lists the root and every loaded import unit, each as a preorder traversal. Two-space indentation records
ownership; the explicit role following `statement` or `expression` records the child edge. Declarations, parameters,
fields, enum variants, statements, expressions, resolved types, stable semantic symbol IDs, operators, folded constants,
type operands, and source spans are included. A dash denotes an absent optional value.

Generic parameter/bound records and specialization identities are explicit. Type arguments appear recursively in type
spellings. Interface and struct methods retain their source ownership; enum payload and match-arm/binding records
describe the new nodes. Template declarations remain visible in AST dumps but are absent from IR. Specialized
declarations include `generic-origin name="..."` and ordered
`type-argument type=...` children so consumers can display source-level generic types instead of private specialization
identifiers. Match bindings include their semantic symbol ID and the name token's source span. These additions
distinguish version 3 from version 2.

Compile-time metadata adds `type-info` expressions with `operand-type` and
`type-property` expressions with a `folded` value. Type-match patterns include
`type=...`; the chosen arm is marked `selected`. Metadata and discarded type arms do not generate runtime IR values or
code.

Async function declarations include `async=1`. `Future<T>` and `JoinHandle<T>` retain their output type in type
spellings; consuming `.await()` appears as an `await` expression. These records currently retain the `dmm-ast-v5`
marker.

## `dmm-symbols-v1`

The symbol dump lists every semantic symbol by stable ID and declaration order. Records include kind, spelling, source
unit, token, owner, scope depth, resolved primitive/named type, pointer depth, array/slice flags, and whether a source
declaration owns the symbol. Concrete struct and enum records also include a `properties=` field containing `COPYABLE` or
`MOVE_ONLY` and, independently, `NEEDS_DROP` when applicable. Generic templates are classified through each concrete
specialization rather than receiving one template-wide property set. The header also reports symbol-index capacity and
occupancy, unresolved expressions, duplicates, and semantic errors. Hash slots are intentionally omitted because their
placement is an implementation detail and may depend on process addresses.

## `dmm-ir-v5`

Version 4 adds `target=elf|coff` and `native-imports=N` to the module header.
Each native import is a separate `native-import #N symbol=... abi=... library=...
name=... return=@N parameters=[...] span=...` record. It never has an IR function
body. Native aggregates add `native opaque=... size=... alignment=...` followed
by `native-field #N offset=... array-stride=...` records. Zero stride denotes a
non-array field. Opaque native structs have zero size/alignment metadata and
cannot be used as complete values. Ordinary aggregate fields retain their slot
representation; native offsets are in bytes.

The IR dump lists interned types and aggregate definitions before functions. In-memory aggregate records retain
the derived ownership properties and whether an explicit destructor exists; these fields drive verification and native
drop-glue generation. `dmm-ir-v5` currently serializes aggregate names, symbols, and fields, but not
those ownership fields; exposing them requires a new dump-format version. Function instructions are numbered in storage
order. Values use `%N`, types use
`@N`, and control-flow labels use `LN`. Each instruction records its opcode, result, type, operands, symbol/token
references, argument slice, operator, and source span. Contextual literals appear as `array-literal` with their pattern
arguments and final element count. Ownership cleanup appears as effectful `drop`, `move`, `reinit`, and
`free-slice-backing` instructions, including inside compiler-generated cleanup/drop-glue functions. Executable main packages also contain
a synthetic package-cleanup function whose reverse-order `drop` effects target `NEEDS_DROP` globals. Enum payload
constants and imported-unit references are explicit.
Numeric constants produced by optimization include `immediate=0x...` with their 64-bit integer value or IEEE
floating-point bits. A fixed-array index may carry `bounds-check=elided` when an earlier dominating access checked the
same SSA base and index. Original source tokens remain unchanged. Dumps describe optimized IR by default; use `-O0` for the
lowered IR. `--dump-ir-before-opt` always captures IR immediately after lowering, so it can be diffed against
`--dump-ir` to explain a transformation. Optimization compacts value and label IDs while preserving source spans.

The current `dmm-ir-v5` writer also emits `future output=@N pinned=1`, `join output=@N pinned=1` and `Executor`
type records. Each async function has an `async constructor=1 poll=1 cleanup=1 pinned=... states=... future=@N send=... sync=0`
metadata line. Instructions include `await`, `executor`, `cancel-check`, `cancel-await`, `cancel-drop` and
`cancel-return`. The dump does not serialize every private in-memory frame/cancellation field or runtime requirement;
see the verifier/runtime contracts for those details. These additions use the v5 marker, so
consumers must support the emitted async records explicitly.

## `dmm-cfg-v1`

The control-flow dump groups the selected IR instructions into basic blocks for each function (`-O0` keeps the lowered
graph). Blocks record reachability,
half-open instruction ranges, predecessor and successor sets, followed by their instruction opcode, result value, and
source span. This is the same CFG construction consumed by verification and optimization, making it suitable for
debugging malformed edges, unreachable code, PHI placement, and branch folding.

## `dmm-source-map-v1`

The source map contains one entry per structured x86-64 instruction emitted by the backend, independent of Intel or AT&T
printing. `instruction N` is the logical instruction ordinal before the conservative assembly cleanup pass. Each entry
names its source-level function and source file. Instructions lowered from IR additionally carry `ir=N` and a 1-based,
end-exclusive source span; compiler-generated prologue, epilogue, cleanup, and ABI instructions use
`ir=- span=-`.

Quoted values use `\\`, `\"`, `\n`, `\r`, and `\t` escapes. Other ASCII control bytes use `\xNN`. Field order, traversal
order, identifier numbering, and final newlines are part of each versioned format. Adding or reordering fields requires
a new format version.

Stage 3 uses v5 AST/IR markers. AST declarations add native-export ABI and native
union/pack/align attributes; callable spellings distinguish `extern system func`.
IR function types print `extern system function`, exports have a `native-export`
record, and aggregates include `native-layout union=... pack=... explicit-align=...`.
The verifier checks these attributes against source declarations and recomputed
layouts. The native-link inventory remains v1; emitted global native function
addresses also contribute their library requirements.