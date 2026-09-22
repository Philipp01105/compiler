# Collections

Import `"stdlib/collections"` for `Deque<T>`, `HashMap<K,V>` and `HashSet<T>`.
Root `"stdlib"` provides `List<T>`, `Buffer<T>` and `Bytes`.

List, Deque and HashMap own move-only elements. Push, insert and set take their
arguments; pop and remove transfer removed values through Option. Replacement,
clear, truncate and destruction destroy exactly the live elements. Growth allocates
before moving any element and uses initialize/take rather than resource byte copies.
Allocation failure preserves existing contents and destroys incoming owner arguments.
Payloads with mandatory consumption, such as pending Futures, cannot be discarded
through a container destructor; complete or cancel them first.

List exposes length, capacity, ensureCapacity, push, insert, set, pop, remove, clear,
truncate, getRef and getMut. Deque is a ring buffer with pushFront/pushBack,
popFront/popBack, clear and the same checked borrow accessors. `deque<T>(capacity)`
returns Result; empty pops return Option.None. Checked access traps on invalid indices.
List insert instead reports InvalidIndex. Copying get/view/append require `core.Copy`;
Buffer remains a default-initializing Copy-only container.

HashMap uses open addressing, tombstones and geometric growth. Existing
`hashMap<K:core.Copy,V>(hash:func(K)->u64,same:func(K,K)->bit)` calls remain valid.
`hashMapBorrowed<K,V>(hash:func(&K)->u64,same:func(&K,&K)->bit)` also supports owner
keys. Callback adapters are ordinary DMM interface values. Constructing an interface
adapter allocates; allocation failure traps, as for other interface conversions.
Callbacks must be non-null and stable; equal keys must have equal hashes.

Map insert takes the key and value. For an equal key, the stored key remains, the old
value is destroyed and the incoming key is destroyed. get requires Copy keys and
values; remove requires Copy keys and transfers the value. getRef/getMut take `&K`
and return `Option<&V>`/`Option<&mut V>`; removeBorrowed takes `&K` and transfers V.
HashSet retains its Copy callback API and explicitly requires `core.Copy`.

Checked borrows keep the container live and prevent relocation, removal, clear or
conflicting access while used. Borrowing payloads transferred into containers retain
their origins conservatively until the container is dropped; clearing individual
slots does not yet release those statically retained loans. Raw Copy views preserve
the existing explicit storage-lifetime contract.

Owner growth, replacement and cleanup are exercised by destruction counters at
O0/O1. Allocation-failure tests inject failures in an isolated stdlib copy and
check preserved contents, successful retries and released allocations. This does
not close the per-slot loan-release limitation above. See
[test coverage](../../tests/stdlib/README.md).
