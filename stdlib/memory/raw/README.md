# Raw memory and lifetime

Import an explicit raw alias for "stdlib/memory/raw". core_alloc(bytes)/core_release(base) use the generated allocator shared with language-owned strings and future frames. Only allocation base pointers may be released, exactly once. core_null, core_offset, core_copy/core_fill and core_load/core_store implement byte-region operations. Zero-byte copy/fill do not touch pointers; nonzero invalid null/count operations trap. Other pointer validity, bounds, alignment and aliasing remain caller responsibilities.

alloc<T>()/alloc<T>(count) zero-fill successful typed allocations; multiplication overflow, zero count and failure produce null/empty output. release(pointer/slice) releases the original allocation; slice copying copies only a descriptor. Raw allocation does not initialize resource values or acquire an automatic destructor. initialize(&storage,value) transfers T into uninitialized raw storage; destroy(&storage) runs its destructor and ends the value lifetime. Reusing initialized storage requires ending the old lifetime first.

core_string_data/core_string_length expose the existing primitive string representation including byte length and embedded NUL. Borrowed string storage must not be mutated or freed. core_exit(status) and core_trap() return never; immediate exit bypasses cleanup.

Native malloc pointers must be freed by their native allocator, never core_release. Prefer Box/Shared, collections and text owners for normal code. The compiler retains layout/ownership/async lowering responsibilities; these explicit source intrinsics preserve the allocator ABI.
