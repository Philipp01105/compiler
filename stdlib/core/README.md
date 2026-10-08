# Canonical core contracts

Import "stdlib/core". This package defines Copy, Send and Sync; Option<T>, Result<T,E>, Propagation<O,R>, NoneResidual, Propagate and FromResidual; AllocError.OutOfMemory/CapacityOverflow; and Iterator<T>.next()->Option<T>.

Option defaults to None. Result requires initialization. Consuming matches transfer payload ownership. mapOption, andThenOption, unwrapOrElse, mapResult, mapError and andThenResult consume their inputs and invoke once callbacks where appropriate. Iterator recognition uses canonical core.Option identity independently of import aliases.

The root stdlib forwards these declarations. Allocation/lifetime operations are in [memory/raw](../memory/raw/README.md), descriptors in [io/raw](../io/raw/README.md), atomics in [sync/atomic](../sync/atomic/README.md), polling in [async/poll](../async/poll/README.md), and owners in their responsible packages. Core has no platform or owning application dependencies.
