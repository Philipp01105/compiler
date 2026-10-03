# Ownership helpers

Import "stdlib/memory". box<T>(value) returns core.Result<Box<T>,core.AllocError>.
Box accepts resource payloads, initializes once, and destroys its payload before
freeing the allocation. get() returns &T and getMut() returns &mut T; their
checked borrows prevent conflicting access or moving the owner.
intoInner() transfers the payload, releases the allocation, and leaves the Box empty.
The empty Box can be destroyed or moved; get/getMut/intoInner on it trap.

shared<T>(value) returns core.Result<Shared<T>,core.AllocError>. Each handle owns
one strong reference; clone() increments the count without copying T. get()
returns a checked shared borrow tied to the handle. The last handle destroys T
and releases its storage. Shared is Send/Sync when T is both Send and Sync.
Allocation failure consumes and destroys the input. Reference-count overflow
traps before wrapping. There are no weak references or implicit clones.

Box is Send when T is Send and Sync when T is Sync. Shared and Box getters use
checked owner origins; raw pointers and allocator details stay private.

arena(capacity) returns Result<Arena,AllocationError>. The fixed byte arena
provides allocate(size) -> Result<offset,...>, region(offset,length), view,
length, capacity and reset. Views borrow the arena; reset/move with a live view
is rejected. Overflow/exhaustion is reported before modifying the cursor.
Only bytes are stored: this API does not promise typed resource destruction.
