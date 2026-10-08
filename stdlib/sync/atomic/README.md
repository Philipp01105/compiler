# Atomic words

Import "stdlib/sync/atomic" (package atomic). atomicBit(initial)/atomicUsize(initial) construct move-only AtomicBit/AtomicUsize. load/store/swap/compareExchange use sequentially consistent ordering. swap and compareExchange return the previous value; equality with expected identifies exchange success. The wrappers are Send and Sync and require no async feature.

Storage remains aligned and cannot be accessed through a non-atomic alias while shared. Native runtime operations implement machine atomics; refactoring their source package does not change ABI symbols.
