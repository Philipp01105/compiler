# stdlib/algorithms

Ordinary DMM slice algorithms; no allocation, OS imports or compiler intrinsics.
Monomorphic callbacks participate in inference, e.g. `algorithms.sort(values,less)`.
Ambiguous callback overloads require explicit type arguments.
Callbacks accept ordinary function values and explicit capturing closures. Repeatedly
called callbacks use `mut func`, including mutable comparator and predicate captures.
Element-valued callbacks, assignments and
swaps require `T: core.Copy`; move-only instantiations are rejected at the call.

| API | Contract |
|---|---|
| find / count / all / any | Predicate-based traversal; find returns Option<usize>. All of an empty slice is true; any is false. |
| equal | Length and element equality through the supplied comparator. |
| reverse | In-place reversal, including empty and singleton slices. |
| sort | In-place heapsort, O(n log n), constant storage, not stable. Comparator must be a strict weak ordering. |
| lowerBound / binarySearch | Sorted input required, same comparator as sort; first equivalent position on duplicates. |
| copy / transform | Checked source/destination descriptor borrows; destination must fit the complete source. Too-small output remains unchanged. Source and destination storage must be disjoint. |
| fill | Assign a copyable value to every output element. |
| fold | Left-to-right reduction; empty input returns the initial accumulator. |

Raw slices still require valid storage. Checked descriptor borrows do not validate
allocation provenance or establish safety for fabricated raw-pointer aliases. Predicate
and comparator functions must not mutate the traversed storage through another alias.
