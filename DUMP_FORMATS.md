# AST, IR, and source-map dumps

The compiler exposes three deterministic, line-oriented formats. Each begins
with a version marker; consumers must reject unknown versions rather than infer
a schema from individual fields.

```sh
compiler --dump-ast program.ast --dump-ir program.ir \
  --source-map program.map -o program.s program.dmm
```

Paths must be distinct from all loaded sources, generated output, and one another. Dump
creation is part of compilation: an I/O or validation failure makes the command
fail and removes the incomplete artifact.

## `dmm-native-map-v1`

Native `--emit=obj` and `--emit=exe` source maps begin with this marker. Records
use the assembly map's instruction, function, source, IR index and span fields,
prefixed with `text-offset N`, the instruction's byte offset in the text section.
Executable offsets cover source-generated code before runtime/startup/import
thunks are appended. They are section offsets, not file offsets or addresses.

## `dmm-ast-v2`

The AST dump lists the root and every loaded import unit, each as a preorder
traversal. Two-space indentation records ownership;
the explicit role following `statement` or `expression` records the child edge.
Declarations, parameters, fields, enum variants, statements, expressions,
resolved types, stable semantic symbol IDs, operators, folded constants, type operands, and source
spans are included. A dash denotes an absent optional value.

## `dmm-ir-v3`

The IR dump lists interned types and aggregate definitions before functions.
Function instructions are numbered in storage order. Values use `%N`, types use
`@N`, and control-flow labels use `LN`. Each instruction records its opcode,
result, type, operands, symbol/token references, argument slice, operator, and
source span. Enum payload constants and imported-unit references are explicit.
Numeric constants produced by optimization include `immediate=0x...` with their
64-bit integer value or IEEE floating-point bits. Original source tokens remain
unchanged. Dumps describe optimized IR by default; use `-O0` for the lowered IR.
Optimization compacts value and label IDs while preserving source spans.

## `dmm-source-map-v1`

The source map contains one entry per structured x86-64 instruction emitted by
the backend, independent of Intel or AT&T printing. `instruction N` is the
logical instruction ordinal before the conservative assembly cleanup pass.
Each entry names its source-level function and source file. Instructions lowered
from IR additionally carry `ir=N` and a 1-based, end-exclusive source span;
compiler-generated prologue, epilogue, cleanup, and ABI instructions use
`ir=- span=-`.

Quoted values use `\\`, `\"`, `\n`, `\r`, and `\t` escapes. Other ASCII control
bytes use `\xNN`. Field order, traversal order, identifier numbering, and final
newlines are part of each versioned format. Adding or reordering fields requires a new
format version.
