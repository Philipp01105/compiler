# Pipeline fuzzing seeds

The CI fuzzers mutate the stage-specific corpus plus the generic execution examples
and the IR corpus (except for the assembly syntax converter).
Seeds are independent source files, not a single package. Parser seeds may contain
unresolved imports, native declarations, or async syntax; the in-memory fuzz harness
does not load a module manifest or enable async. Semantic seeds deliberately include
invalid programs to reach constraint, lifetime, and borrow diagnostics.

The following IR seeds are self-contained and must compile at O0 and O1 so mutations
can reach lowering, optimization, and verification rather than stop at resolution:

| Seed | Combined features |
| --- | --- |
| `auto_properties_specializations` | External primitive and concrete specialization rules, conditional properties, nested generic structs, enum and array payloads, raw allocation |
| `explicit_lifetime_branch_cleanup` | No-default generics, branch joins, move/reinitialization, tracked and heap initialize/destroy, default enum cleanup, deferred mutable borrow |
| `dynamic_interfaces_nested_sums` | Structural constraints, erased interface arrays, owning interface fields, generic sum types, dynamic dispatch and loops |
| `callable_generic_control_values` | Polymorphic callable identities, function arrays, enum callable payloads, unbound methods and expression-valued blocks/if/match |
| `nested_arrays_borrow_regions` | Nested fixed arrays, disjoint mutable references, checked slices/subslices, constants, casts and snapshot copying |
| `move_only_enum_control_flow` | Move-only generic aggregates, destructor propagation, conditional moves, early returns, deferred cleanup and sum-type control flow |
| `numeric_loops_deferred_control` | Nested loop break/continue, per-iteration defer, value conditionals, enum common fields and chained numeric conversions |

The additional diagnostic seeds combine conditional property failures and cycles,
mixed initialization states and conflicting loans, and specialized structural
interface mismatches. Parser surface seeds cover Shared task cancellation, native
callbacks with opaque storage, and malformed nested generic/type boundaries.

`fuzz_corpus_contract` keeps the seven valid IR seeds compilable at both optimization
levels and checks that the diagnostic seeds are rejected. Fuzzer smoke runs still
use sanitizers and their existing per-input timeout.
