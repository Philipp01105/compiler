# Iterator conveniences

core.Iterator<T> is the fundamental next()->Option<T> protocol. Collection
iter/iterRef/iterMut methods supply the compiler's for-loop conventions.

Import `"stdlib/iter"` for eager map/filter/enumerate and allocation-free fold.
Eager results are owned List values and report allocation errors. The iterator is
mutably borrowed and advanced; yielded items are consumed and need not be Copy. Borrowed items retain their origins.
These are a compact convenience layer, not a lazy adapter framework. Mutable
lending iteration is intended for per-item processing, not retaining aliases.
