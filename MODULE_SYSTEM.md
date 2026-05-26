# Modules, packages and imports

DMM 0.3 uses one module per project, one package per directory, and one or more
`.dmm` source files per package. Subdirectories are separate packages.

```text
project/
├── dmm.mod
├── cmd/compiler/main.dmm       package main;
├── cmd/formatter/main.dmm      package main;
├── lexer/lexer.dmm             package lexer;
├── lexer/scanner.dmm           package lexer;
└── internal/buffer/buffer.dmm  package buffer;
```

The compiler searches upward from the selected source file or package directory
for `dmm.mod`. A manifest is not DMM source code. Its module path is the project's
global identity; the directory relative to its root completes a package's identity.
Nested module roots are excluded from the enclosing module's packages.

```text
module github.com/example/compiler

dmm 0.3

require (
    github.com/example/collections v1.2.0
)
```

There must be exactly one `module` directive and one `dmm` directive. Module and
package paths are case-sensitive slash-separated identifiers. Empty components,
`.` and `..`, backslashes and absolute paths are invalid. This compiler supports
language edition `0.3`; incompatible editions are rejected. Dependency versions
currently use `vMAJOR.MINOR.PATCH`. Duplicate requirements and malformed directives
are rejected. `//` comments and single-line `require path version` are supported.

External dependencies are local for now. `require github.com/example/collections
v1.2.0` authorizes loading that module from
`vendor/github.com/example/collections/`, which must contain its own matching
`dmm.mod`. Missing requirements, missing vendored sources and conflicting module
identities are errors. Transitive requirements are read from each dependency's
manifest and use the same project-root `vendor/` directory. The version records
the requested dependency; there is no network fetch, version selection, checksum
validation or lockfile yet. The bundled
`stdlib` module is available independently of project requirements.

Every source file begins with `package name;` before imports or other declarations.
Comments and whitespace may precede it. All source files directly in a package's
directory must have the same local name. Discovery includes every regular `.dmm`
file directly in that directory, including an ordinary file named `package.dmm`;
that filename has no special role. Other extensions and subdirectories are ignored.
Files are parsed before package symbol collection, and private declarations are
visible throughout their package, regardless of source-file order.

```dmm
package parser;

import lex "github.com/example/compiler/lexer";

pub func tokenKind(token:lex.Token) -> int {
    return token.kind;
}
```

Imports bind a package in the importing source file. Without an explicit alias,
the binding uses the imported package's declared local name. Access is qualified:
`lexer.Token`, `lexer.tokenize(...)`, or `lex.Token` with an alias. Imports in one
source file do not create import bindings in another file of the same package.
The existing grouped form remains available: `import ("path/a" b "path/b");`.
Dot imports, blank aliases, duplicate aliases and file imports are unsupported.

Declarations and members are private by default. `pub` applies to functions,
structs, enums, traits, constants and package variables. Fields and methods require
their own `pub`; exporting a struct does not export its private members. Enum
variants also require `pub`, for example:

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

Package variables have shared writable storage. Initializers currently require
constant primitive or string expressions; explicitly typed variables may instead
be zero-initialized, including arrays, pointers and aggregate values. Runtime
initialization functions and arbitrary package initializer expressions are future
work. Existing parameter-only slice rules remain in force.

Only `package main` is executable. It requires exactly one non-generic,
parameterless `main` returning `int` or `void`. Other packages are libraries and
can emit assembly or relocatable objects without an entry point. Executable builds
of a library are rejected; executable packages cannot be imported. Multiple
`cmd/...` packages named `main` can coexist in one module.

For every `internal` path segment, the importing package must be within the
subtree of that segment's parent and belong to the same module. Thus
`example.com/lib/io/internal/syscall` is accessible under `example.com/lib/io`,
but not from `example.com/lib/parser` or another module. Package import cycles are
compile-time errors and report the complete path through the cycle.

Symbol identity includes the canonical package path (which includes the module
path), the declared name and, where applicable, the owner type and overload or
specialization signature. Identically shaped types from different packages remain
nominally distinct. Backend names encode canonical package identity; public DMM
names are not automatically unmangled C names. Interoperability callers must use
the generated link name. Generic identities use declaration names and signatures,
not source-file token positions.

The frontend owns `DmmModule`, `DmmPackage` and source ASTs. The loader builds the
package graph and loads all source files; semantic analysis collects package-wide
symbols, resolves file-local import bindings and checks visibility. Typed IR retains
source/package identity, nominal type IDs, imports and package variable storage.
The backend uses those identities for mangling and emits executable startup/runtime
only for executable packages. AST/IR dumps include canonical package identities;
diagnostics use the existing human-readable and JSON formats.

Migration: replace angle or file imports with quoted package paths, add `dmm.mod`
and `package name;`, qualify imported declarations, and mark exported APIs and
members with `pub`. Remove `package.dmm` import lists; discovery supplies the
package's files automatically. See the standalone modules under `examples/` and
the multi-package integration cases in `tests/imports_test.cmake`.
