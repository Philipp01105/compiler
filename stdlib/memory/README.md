# Ownership helpers

Import "stdlib/memory". box<T>(value) returns Result<Box<T>,AllocationError>.
Box accepts resource payloads, initializes once, and destroys its payload before
freeing the allocation. get() returns &T and getMut() returns &mut T; their
checked borrows prevent conflicting access or moving the owner.
intoInner() transfers the payload, releases the allocation, and leaves the Box empty.
The empty Box can be destroyed or moved; get/getMut/intoInner on it trap.

arena(capacity) returns Result<Arena,AllocationError>. The fixed byte arena
provides allocate(size) -> Result<offset,...>, region(offset,length), view,
length, capacity and reset. Views borrow the arena; reset/move with a live view
is rejected. Overflow/exhaustion is reported before modifying the cursor.
Only bytes are stored: this API does not promise typed resource destruction.
