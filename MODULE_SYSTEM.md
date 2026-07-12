# Modules, packages and imports

DMM 2026-09-22-dev uses one module per project, one package per directory, and one or more
`.dmm` source files per package. Subdirectories are separate packages.

```text
project/
├── dmm.manifest
├── cmd/compiler/main.dmm       package main;
├── cmd/formatter/main.dmm      package main;
├── lexer/lexer.dmm             package lexer;
├── lexer/scanner.dmm           package lexer;
└── internal/buffer/buffer.dmm  package buffer;
```

The compiler searches upward from the selected source file or package directory for `dmm.manifest`. A manifest is not
DMM source code. Its module path is the project's global identity; the directory relative to its root completes a
package's identity. Nested module roots are excluded from the enclosing module's packages.

```text
module github.com/example/compiler

dmm 2026-09-22-dev

features = []

require (
    github.com/example/collections v1.2.0
)
```

There must be exactly one `module` directive and one `dmm` directive. Module and package paths are case-sensitive
slash-separated identifiers. Empty components, `.` and `..`, backslashes and absolute paths are invalid. Edition syntax,
validation and compatibility policy are defined in [LANGUAGE_SPEC.md](LANGUAGE_SPEC.md); this document uses the current
edition in its examples.

The optional `features = [...]` directive contains quoted experimental feature names. It may span lines, permits a
trailing comma, and is equivalent to omission when empty. Names begin with a lowercase ASCII letter and continue with
lowercase letters, digits, or underscores. Unknown and duplicate features are rejected. The manifest is authoritative;
source files and normal command-line builds cannot override it.

Dependency versions currently use `vMAJOR.MINOR.PATCH`. Duplicate requirements and malformed directives are rejected.
`//` comments, non-nesting `/* ... */` block comments and single-line `require path version`
are supported. An unclosed block comment invalidates the manifest.

Package documentation lives in its enclosing module's manifest. Start a block with the exact source package name as its
first word. Each block documents one package; editor hovers select only the matching block, without merging other
comments:

```text
/* lexer
Tokenizes DMM source files.
Provides tokens and source positions for the parser.
*/
```

Function documentation is the immediately preceding `//` comment group or
`/* ... */` block in the source file. JetBrains hovers show it with the declared signature, including generic
parameters, parameter types and the return type.

External dependencies are local for now. `require github.com/example/collections
v1.2.0` authorizes loading that module from
`vendor/github.com/example/collections/`, which must contain its own matching
`dmm.manifest`. Missing requirements, missing vendored sources and conflicting module identities are errors. Transitive
requirements are read from each dependency's manifest and use the same project-root `vendor/` directory. The version
records the requested dependency; there is no network fetch, version selection, checksum validation or lockfile yet. The
bundled
`stdlib` module is available independently of project requirements.

Use one command to reconcile dependencies:

```sh
dmm manifest sync
dmm manifest sync path/to/package
```

The command finds the enclosing module and scans all of its packages, including libraries and every executable under
`cmd/`. Nested modules, hidden directories and unimported vendor packages are excluded. It adds used transitive modules,
marks them with `// indirect`, promotes directly imported modules to ordinary requirements and removes unused
requirements. Direct means imported by any source package in the project; indirect means imported only by its
dependencies.

Versions come from existing project and dependency requirements. Missing explicit versions, unavailable vendor sources,
invalid packages, import cycles and conflicting exact versions abort synchronization without changing the project
manifest. This command does not fetch packages or select versions. Build commands read manifests and do not rewrite
them. `--formatError` provides structured failure diagnostics.

Synchronization writes a sorted, normalized manifest and replaces the original only after successful graph analysis and
writing. Block comments are preserved and placed before the directives, so package documentation survives
synchronization. Other formatting and comments except `// indirect` are not preserved. Repeated synchronization is
deterministic.

```text
require (
    example.com/direct v1.0.0
    example.com/transitive v2.0.0 // indirect
)
```

Every source file begins with `package name;` before imports or other declarations. Comments and whitespace may precede
it. All source files directly in a package's directory must have the same local name. Discovery includes every regular
`.dmm`
file directly in that directory, including an ordinary file named `package.dmm`; that filename has no special role.
Other extensions and subdirectories are ignored. Files are parsed before package symbol collection, and private
declarations are visible throughout their package, regardless of source-file order.

```dmm
package parser;

import lex "github.com/example/compiler/lexer";

pub func tokenKind(token:lex.Token) -> int {
    return token.kind;
}
```

Imports bind a package in the importing source file. Without an explicit alias, the binding uses the imported package's
declared local name. Access is qualified:
`lexer.Token`, `lexer.tokenize(...)`, or `lex.Token` with an alias. Imports in one source file do not create import
bindings in another file of the same package. The existing grouped form remains available:
`import ("path/a" b "path/b");`. Dot imports, blank aliases, duplicate aliases and file imports are unsupported.

Declarations and members are private by default. `pub` applies to functions, structs, enums, interfaces, constants and
package variables. Fields and methods require their own `pub`; exporting a struct does not export its private members.
Enum variants also require `pub`, for example:

```dmm
package result;

pub enum Result<T,E> { pub Ok(T), pub Err(E), }
pub struct Cell<T> {
    pub var value:T;
    var cached:int;
    pub func get() -> T { return value; }
}
pub var count:int = 0;
```

Ownership is part of the resolved concrete type, not its export spelling. Imported structs therefore retain their
derived `COPYABLE`/`MOVE_ONLY` and independent `NEEDS_DROP` properties across package boundaries. Generic structs
derive those properties separately for each concrete specialization after type substitution.

For an executable main package, globals from the resolved package graph that require destruction participate in the
compiler-generated normal-exit cleanup and are dropped in reverse lowered declaration order. Library packages do not
emit executable startup. Moving an owner out of package storage is rejected; borrowing and in-place reassignment remain
available under the ordinary borrow and exactly-once drop rules.

Package variables have shared writable storage. Initializers currently require constant primitive or string expressions;
explicitly typed variables may instead be zero-initialized, including arrays, pointers and aggregate values. Runtime
initialization functions and arbitrary package initializer expressions are future work. Slice variables store a
pointer/count descriptor; they may be zero-initialized and assigned at runtime. Views do not own or extend the lifetime
of their storage.

Only `package main` is executable. It requires exactly one non-generic, parameterless `main` returning `int` or `void`.
Other packages are libraries and can emit assembly or relocatable objects without an entry point. Executable builds of a
library are rejected; executable packages cannot be imported. Multiple
`cmd/...` packages named `main` can coexist in one module.

For every `internal` path segment, the importing package must be within the subtree of that segment's parent and belong
to the same module. Thus
`example.com/lib/io/internal/syscall` is accessible under `example.com/lib/io`, but not from `example.com/lib/parser` or
another module. Package import cycles are compile-time errors and report the complete path through the cycle.

Symbol identity includes the canonical package path (which includes the module path), the declared name and, where
applicable, the owner type and overload or specialization signature. Identically shaped types from different packages
remain nominally distinct. Backend names encode canonical package identity; public DMM names are not automatically
unmangled C names. Interoperability callers must use the generated link name. Generic identities use declaration names
and signatures, not source-file token positions.

The frontend owns `DmmModule`, `DmmPackage` and source ASTs. The loader builds the package graph and loads all source
files; semantic analysis collects package-wide symbols, resolves file-local import bindings and checks visibility. Typed
IR retains source/package identity, nominal type IDs, imports and package variable storage. The backend uses those
identities for mangling and emits executable startup/runtime only for executable packages. AST/IR dumps include
canonical package identities; diagnostics use the existing human-readable and JSON formats.

Migration: replace angle or file imports with quoted package paths, add `dmm.manifest`
and `package name;`, qualify imported declarations, and mark exported APIs and members with `pub`. Remove `package.dmm`
import lists; discovery supplies the package's files automatically. See the standalone modules under `examples/` and the
multi-package integration cases in `tests/imports_test.cmake`.
