# Ownership helpers

Import "stdlib/memory". box(value) returns core.Result<Box<T>,core.AllocError>. Box initializes once and destroys its payload before freeing storage. get()/getMut() return checked references that block conflicting access or moving the owner. memory.intoInner(box) consumes the Box and transfers its payload while releasing storage.

shared(value) returns core.Result<Shared<T>,core.AllocError>. clone() increments its strong count without copying T; get() borrows the payload from the handle. The last handle destroys T and releases storage. Allocation failure destroys the incoming payload. Refcount overflow traps before wrapping. There are no weak references or implicit clones.

Box is Send when T is Send and Sync when T is Sync. Shared is Send/Sync when T is both Send and Sync. Allocator details remain private. Explicit raw allocation and initialize/destroy belong to [memory/raw](raw/README.md). Use collections.Buffer<u8> or List<u8> for byte storage.
