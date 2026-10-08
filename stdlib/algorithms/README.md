# stdlib/algorithms

Ordinary DMM slice algorithms; no allocation, OS imports or compiler intrinsics.
Monomorphic callbacks participate in inference, e.g. `algorithms.sort(&mut values,less)`.
Ambiguous callback overloads require explicit type arguments.
Callbacks accept ordinary function values and explicit capturing closures. Repeatedly
called callbacks use `mut func`, including mutable comparator and predicate captures.
Element-valued callbacks, assignments and
swaps require `T: core.Copy`; move-only instantiations are rejected at the call.
Borrowed traversal instead accepts `&T` elements and supports move-only owners.

| API | Contract |
|---|---|
| find / count / all / any | Take &T[] and a mut func(&T)->bit, including move-only elements; find yields the first Option<usize>. find/any/all short-circuit. all(empty) is true; any(empty) is false. |
| forEachMut | Takes `&mut T[]` and a `mut func(&mut T)->void`; mutates each element in place, including move-only owners, in index order. |
| equal | Length and element equality through the supplied comparator. |
| reverse | Checked &mut slice; in-place reversal, including empty and singleton slices. |
| sort | Checked &mut slice; in-place heapsort, O(n log n), constant storage, not stable. Comparator must be a strict weak ordering. |
| lowerBound / binarySearch | Sorted input required, same comparator as sort; first equivalent position on duplicates. |
| copy / transform | Checked source/destination descriptor borrows; destination must fit the complete source. Too-small output remains unchanged. Source and destination storage must be disjoint. |
| fill | Assign a copyable value to every output element. |
| fold | Left-to-right reduction; empty input returns the initial accumulator. |

Raw slices still require valid storage. Checked descriptor borrows do not validate
allocation provenance or establish safety for fabricated raw-pointer aliases. Predicate
and comparator functions must not mutate the traversed storage through another alias.

Callbacks may capture mutable counters or owned state. A callback is passed by value;
move-only captures transfer into the algorithm and are cleaned up once afterward.
An element borrow does not permit taking its owner or extending its storage lifetime.
Named fixed arrays converted to slice views retain ownership and element cleanup.
