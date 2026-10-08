# Collections

Import `"stdlib/collections"` for List<T>, Buffer<T>, Map<K,V> and Set<T>.
Factories and allocating methods return core.Result with explicit allocation
errors. Empty List is allocation-free; list<T>() is its ordinary constructor.

List append/insert consume elements; pop/remove return Option<T> and transfer
ownership. ensureCapacity, clear, truncate, length and capacity cover storage
management. get/getMut return checked references and trap on invalid indices;
insert traps when index exceeds length. appendSlice copies only Copy elements.
Growth allocates before moving live elements. Failure preserves stored elements
and destroys consumed incoming arguments once. Destruction drops live elements.
Pending Futures with mandatory consumption must be completed/cancelled first.

Buffer is fixed-size owning storage, including move-only elements.
buffer(count,initial) requires Copy; bufferWith(count,init) creates each element
with a mutable callback. get/getMut are checked; length reports element count.
List/Buffer view exposes a Copy-element slice with a tracked receiver origin;
it cannot escape its owner or coexist with relocating mutation. Raw slice
writability otherwise follows DMM's existing descriptor semantics.

for(var item=values) uses Copy iteration; for(var &item=values) borrows elements;
for(var &mut item=values) provides per-iteration mutable references. iter,
iterRef and iterMut expose these protocols. Mutable items cannot escape a loop
iteration. collections.contains(&list,&value) uses the default equality policy;
list.contains(&value,equal) supports explicit borrowed equality for arbitrary T.

map<K,V>() uses supported primitive/string policies. map(hash,equal) accepts
noncapturing borrowed callbacks. Equal keys must hash equally. seededMap accepts
a keyed hash callback and obtains per-owner entropy. Default policies use
SipHash-2-4, whose explicit low-level implementation is in collections/hash.
text.map<V>() supplies owned String keys. Map insert consumes K/V; replacing a
value preserves the stored key and destroys the incoming equivalent key.
get/getMut borrow V from the Map, independently of the temporary lookup key.
remove transfers V. iter yields borrowed EntryRef; entriesMut lends EntryMut.
clear destroys entries but retains capacity. Set wraps these ownership policies;
remove transfers the actual stored T. iterRef borrows stored values.

Conditional Send/Sync depends on element/key/value types. Checked borrows block
owner destruction, relocation and conflicting access. Dynamic key/index
relationships remain conservative. Allocation-fault and ownership contracts
cover failed growth, replacement, cleanup, rehash and transferred payload loans.
