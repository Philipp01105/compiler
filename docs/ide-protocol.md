# IDE workspace protocol

The current JetBrains language service requires `dmm-ide-v1`. Earlier compiler interfaces are not a compatibility contract for this service.

## Transport

`dmm --ide-capabilities` writes a JSON capability object to stdout. Supported capabilities currently include `workspace-analysis`, `workspace-index`, `implementations`, `manifest-overlays`, `semantic-values`, `completion`, `rename-local`, `manifest-sync-preview`, `function-signatures`, `type-inlays` and `call-graph`. Position encoding is UTF-16. No native debug format or general refactoring preservation capability is advertised.

`dmm --ide-request request.json --ide-response response.json` analyzes a workspace without emitting code or changing source/manifest inputs. The response must use a `.json` path separate from every input. Protocol failures exit with status 2 and an error response when the response path is safe. Compilation diagnostics are successful protocol results, with exit status 0.

## Analysis request

```json
{
  "protocol": "dmm-ide-v1",
  "requestId": "snapshot-42",
  "operation": "analyze",
  "root": "file:///workspace/main.dmm",
  "target": "elf",
  "overlays": [
    {"uri": "file:///workspace/main.dmm", "version": 42, "text": "package main; func main()->int{return 0;}"}
  ],
  "manifestOverlays": []
}
```

`target` is optional (`elf` or `coff`, defaulting to the host). URIs must identify local files; percent escapes are decoded before canonicalization. Duplicate canonical overlays are rejected. Overlays use nonnegative integer versions, and only `.dmm` source files and `dmm.manifest` manifests are accepted. Unsaved package files can participate when their package directory exists.

`stdlibRoot` optionally selects a standard-library source directory containing the `stdlib` module manifest. Analysis, completion and rename validation use that same provider; an invalid configured provider produces import diagnostics instead of falling back to another distribution. An empty string uses compiler discovery.

`workspace-analysis` discovers every source package in the root module, including unopened reverse callers, before semantic analysis. Compiler target filtering and nested module boundaries apply, including dirty manifest overlays. Dot directories and `vendor` are excluded from own-module discovery; imported vendored packages are loaded through ordinary import resolution. Discovery is bounded to 4,096 directories. A failed or limited scan is a protocol failure. Successful responses include `workspaceDiscoveryComplete: true`, the module-directory URI in `workspaceRoot` and one selected source-file URI per module package in `workspaceRoots`. Ordinary `analyze` returns `workspaceDiscoveryComplete: false`. Complete reverse discovery across independent external modules remains unfinished.

`directory-inventory` adds `directoryDependencies` with directory URI, `boundaryOnly` and `fnv1a64-inventory` hash. Ordinary inventories include nonhidden, nonvendor child directories, `.dmm` files and `dmm.manifest`; nested-module inventories include only the manifest entry. Each entry contributes FNV-1a of its UTF-8 name, NUL and kind (`d`, `f`, `o`) to an unsigned 64-bit sum initialized to the FNV offset basis. The hash is `(sum XOR entryCount) * FNVPrime` modulo 2^64. Enumeration order does not affect it. Inventories are bounded to 8,192 records and 10,000 raw entries per directory. The compiler rechecks inventories before exporting a graph and includes their fingerprints in `snapshotId`. Clients combine these with file content fingerprints and overlay versions when caching or accepting usage results.

Request size is limited to 32 MiB, with 1,024 total overlays. The JSON parser rejects duplicate keys, malformed numbers and invalid Unicode escapes. Saved file contents, including missing files, are frozen on first read for the request. The source cache is bounded to 64 MiB and 10,000 paths; cache capture failures reject analysis rather than silently returning incomplete results.

## Response

The response echoes `protocol` and `requestId`. It includes:

- `snapshotId` and `dependencies` with local file URIs and `fnv1a64` content fingerprints.
- `diagnostics` with URI, code, severity, UTF-16 range, message and optional replacement fix.
- `declarations` with snapshot-local ID, source identity/name range, kind, owner, display type, scope and type/borrow properties.
- Function declarations include ordered `parameters`, `genericParameters`, `lifetimeParameters`, `returnType`, `async` and full `sourceRange`. Inferred variable types come from checked semantic values and carry `inferred: true`.
- `occurrences` with URI, range, semantic role and declaration ID; unresolved links are null.
- Occurrences in ordinary and checked generic function bodies include a `caller` declaration ID. Direct resolved calls support snapshot-local call hierarchy, including unopened own-module packages for `workspace-analysis`.
- `completionResolved` and `items` for completion requests.
- `diagnosticsJson` and `ast` for internal editor consumers that still read compiler diagnostic/AST dumps. These fields do not imply support for older compiler protocols.

Generated/generic graph coverage is still being developed. Clients must not infer refactoring safety from a successful compile or from lexical matches. Snapshot-local numeric IDs are not stable across requests.

Original generic declarations, members, parameters and locals are registered even without a checked instance. `type-parameter` and `lifetime-parameter` declarations have source anchors and scope IDs; nested callable binders have independent scopes. Scope ranges and local visibility preserve shadowing and initializer lookup. `sourceOnly: true` distinguishes source entries from runtime symbols. Source-only inferred types are `unknown`, and clients suppress their inlays.

Checked generic instances share source declaration IDs using explicit AST provenance and exact source anchors. Source signatures preserve template spellings. Numeric IDs are snapshot-local; `identity` uses the source path, symbol kind and name location and is independent of the analysis root. Source locations can change after edits. Substituted parameters do not create artificial source references to their concrete types.

Anonymous closure declarations use the source `func` token as `nameRange`, carry `anonymous: true` and `displayName: "<closure>"`, and retain their source owner, parameters and body range. Generated capture fields and consuming match bindings link back to the captured source declaration. Generated closure invocation calls retain the original callee name span and link to the source closure, including borrowed and consuming calls. Nested closure scopes allow only explicit captures when searching for outer value bindings. Async parameters and locals retain source callable ownership.

Function declarations export `static`, `declaredReceiverMode` and `receiverMode` (`0` shared, `1` mutable, `2` consuming, null when unknown). Closure-valued source variables display `func`, `mut func` or `once func` signatures instead of generated environment names. Compound assignment carries read and write occurrences; mutable capture writes point to the original captured binding.

Occurrences additionally carry compiler metadata when applicable:

| Field | Meaning |
|---|---|
| `ownershipOperation` | `1` initialize, `2` destroy, `3` take, `4` replace |
| `accessBorrowKind` | `1` immutable borrow, `2` mutable borrow |
| `implicitReceiver` | The source use accesses an implicit receiver field |
| `asyncCall` | A call to an async function |
| `await` | A use within the operand of an await expression |
| `asyncOperation` | The compiler's `AstAsyncOperation` enum value for a lowered async operation; this is separate from `asyncCall` |
| `intrinsic` | A recognized compiler intrinsic, which may have no source declaration |
| `sourceCandidate` | A compiler candidate whose selection is not proven for every source context |

Members in unused templates can resolve through declared interface bounds and nominal generic owners without creating a specialization. Source lookup preserves owner identity, visibility, callable arity, local aliases and explicit pointer dereferences. Multiple viable bound members remain separate candidates; the unresolved source selection also remains in the coverage audit. Candidate discovery does not imply complete overload or generic-substitution proof.

`implementationRelations` contains compiler-checked structural interface/type pairs with `interface`, canonical `implementation`, `checkedImplementation` and method pairs (`required`, `implementation`). Generic bounds retain each distinct checked or inferred argument tuple in `interfaceContexts` (`id`, source `interface`, `argumentTypes`); their relationships also carry `interfaceContext`. These IDs are snapshot-local. A generic declaration query returns the union of these contexts, retaining separate context facts in the protocol rather than selecting one specialization. Signature substitution, argument arity, the interface's own parameter bounds, method constraints and inferred receiver contracts participate in checking. Imported bound arguments retain their declaration unit when copied into the checking unit.

Unused generic interfaces infer distinct argument tuples from candidate signatures, branching over overloads and checking every required method. Substitution retains reference, pointer, slice, array and callable shapes. A parameter unused by required method signatures exports `"*"` and its zero-based bit in `interfaceContexts.wildcardParameters`, including when that parameter has bounds. `parameterBounds` contains an ordered array of bound-name arrays, one per interface parameter, preserving the wildcard's valid domain. A wildcard quantifies over that domain; it is not a selected concrete witness. Dependent bounds still require a proof and can make discovery incomplete. Source aggregate contexts export `sourceOwner`, preserving the identity of their own generic binders. Declared owner bounds can discharge interface parameter bounds; missing or conditional proofs report incomplete coverage. Source receiver checks respect local shadowing and reject unknown receiver effects. Generic method, lifetime and `where`-constraint proofs remain incomplete in source-template discovery.

`implementationCoverage.ordinary` reports whether the bounded relation scan completed. `checkedGenericInterfaces` additionally requires complete context collection and error-free semantic checking. `uncheckedInterfaces` lists generic interfaces whose discovery proof is incomplete; both adapters reject implementation/member/subtype queries for these declarations with an explained coverage failure. `genericInterfaces` is true only when all generic-interface discovery proofs completed. Closure-environment implementations are excluded. The context inventory is limited to 4,096 argument tuples and inference/export scans to 50,000 work steps/candidate pairs; a limit or context inventory change during export invalidates completeness. Relationships use semantic signature compatibility and receiver rules, not matching method names alone.

`sourceRegistryComplete` reports bounded source registration. `bindingCoverage.templateDeclarations` reports that same registration coverage. A separate source-use audit exports `auditComplete`, `complete`, `generatedBindings`, `contextDependentBindings`, `sourceUseCount` and `uncovered` ranges with URI, role, optional caller and reason. Uses must link to compiler declarations with source anchors, except recognized `intrinsic: true` uses, which may have a null declaration. An unresolved template use remains uncovered even when checked contexts or source candidates provide some alternatives. Import namespace prefixes and intrinsic boolean literals are not binding uses. `sourceUseCount` counts observations, including repeated checked contexts, rather than unique source ranges. The audit is bounded to at most 1,000,000 observations; resource exhaustion invalidates audit completeness independently of traversal. Generated coverage refers to source-bearing uses in the exported graph, not arbitrary synthetic runtime nodes. Complete coverage alone does not establish refactoring preservation.

References/Find Usages require complete binding coverage. Incoming direct-call queries require all source call bindings; outgoing direct-call queries require coverage for the selected callable. Both adapters share this contract and return explained failures for missing coverage. Native hierarchy presents the failure in its tree. LSP hierarchy retains query errors instead of converting them to successful empty answers. Checked-context navigation still exposes all known alternatives and identifies their checked signatures.

When a queried source use has no known declaration target, navigation checks its uncovered range. LSP definition reports a query error and JetBrains Go to Declaration shows an error hint for missing coverage. Known checked-context or source-candidate targets remain available as alternatives; their presence does not make reference or call coverage complete.

Checked specialization occurrences also carry `checkedContext`, referring to an entry in `checkedContexts` with snapshot-local `id`, canonical `sourceDeclaration`, concrete `parameterTypes` and `returnType`. Multiple checked contexts may resolve one source range to different declarations. Consumers preserve those alternatives: LSP definitions/hierarchy preparation return all checked source targets, hover identifies the checked signatures, and JetBrains Go to Declaration offers multiple targets. Singular reference resolution remains unavailable for an ambiguous range. This metadata covers checked callable instances; it does not establish exhaustive context-dependent coverage for other generated constructs.

## Completion

Set `operation` to `complete` and supply a source `uri` and zero-based UTF-16 `position`. The compiler replaces the current identifier with an internal marker in memory and analyzes its active scope or receiver. Completion items carry a declaration ID, label, kind, display detail and original-document replacement range. Positions outside the document or inside surrogate pairs are rejected.

`completionResolved: false` means the analyzer did not resolve the query context. It is not authorization to invent receiver members. Generic, expected-type and auto-import coverage is not yet complete.

## Validated local rename

Set `operation` to `rename` and supply `uri`, zero-based UTF-16 `position` and `newName`. `rename-local` covers local variables and parameters in ordinary, nongeneric, synchronous functions. Native functions, generic/lifetime-dependent functions, closure-bearing source files, implicit receivers and global/type declarations are rejected conservatively.

The compiler constructs exact declaration/use edits, analyzes the proposed workspace in memory, compares every resolved occurrence and symbol's type/ownership facts, and requires identical lowered IR after excluding the renamed spelling and source positions. IR verification retains its source spans. A successful compilation alone does not authorize edits. Binding capture, conflicts and unknown preservation results produce `rename.validated: false` with a conflict explanation and no edits.

Successful responses contain `rename.validated: true` and edits with URI, original range, `oldText` and `newText`. The compiler does not apply edits. Clients must check the document/workspace versions before applying them. The JetBrains handler offers an explicit before/after preview and applies accepted edits as one undoable command; standard LSP rename returns a versioned document edit.

## Manifest synchronization preview

`sync-manifest` accepts a source or manifest root and dirty overlays. It scans the containing module's packages across target variants, resolves the local vendored dependency graph using compiler rules, and reconciles exact requirement versions. Nested module boundaries, including dirty manifests, are respected. User line/block comments are retained; generated indirect annotations are reconstructed from the graph. Conflicting versions and missing providers produce diagnostics and an unvalidated plan.

The response contains `manifestSync` with `validated`, manifest `uri`, `oldText` and `newText`. Preview generation never creates or replaces a project manifest or its `.tmp` file. Dependencies include captured manifest/source fingerprints. The JetBrains Build action shows a before/after preview, rejects stale workspace snapshots and applies accepted document changes as one undoable command. This reconciles local dependencies; it does not download packages.

## Tests

Run `python tests/workspace_ide_test.py <compiler>` against a freshly built compiler. `source_snapshot_unit` tests frozen reads, missing files, overlay precedence and resource-limit failures. The JetBrains standalone-service tests add end-to-end workspace lifecycle/query coverage when `DMM_COMPILER` is set.

The latest local verification on 2026-10-11 used the matching compiler from `cmake-build-ide-validation`:

| Check | Passing evidence |
|---|---|
| Compiler workspace protocol | 51 tests |
| JetBrains platform fixtures | 22 tests, zero skips |
| Standalone LSP fixtures | 21 tests, zero skips |
| Selected compiler CTest checks | 10 checks |

The selected CTest checks were `language_foundation_unit`, `frontend_ast_unit`, `async_semantic_unit`, `async_frontend_contract`, `ide_analysis`, `auto_deref_contract`, `async_runtime_contract`, `async_executor_contract`, `resource_fault_unit` and `source_snapshot_unit`. Protocol fixtures cover inferred and checked interface contexts, bounded wildcards, declared-bound entailment, incomplete receiver proofs, closure/async source provenance, ownership metadata, unused-template members and ambiguous binding coverage. Adapter fixtures verify that missing coverage becomes a query failure and known alternatives remain navigable.

These results are development evidence, not completion of the 1.0 acceptance gates. Directory-root discovery across configured independent modules, exhaustive source-template proofs, contextual completion/import proposals, shared `validate-edits`, general refactoring, and accepted ELF/PE source debugging remain unfinished. The acceptance ledger is maintained in the companion plugin's `docs/remaining-features-plan.md`; the plugin remains `0.9.0-dev`.
