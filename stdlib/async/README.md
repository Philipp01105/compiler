# stdlib/async

`import ("stdlib/async");` exposes the package as `asynchronous`.

`Waker` owns a retained scheduler context. `clone()` acquires another reference,
`wake()` queues the task if it is still active, and destruction releases the
reference. Its raw context is private, so application code cannot forge a
scheduler waker. `noopWaker()` provides a safe no-op value for synchronous
pollers and tests. `Context` owns a waker and `retainWaker()` gives a poller a
clone that can outlive the call.

`Poller<T>` is a structural DMM interface: `poll(&mut Context)` returns
`core.Poll<T>`; `cancelPoll(&mut Context)` returns `core.Poll<CancelAck>`.
`Ready(Confirmed)` promises that child work and wake registrations have ended.
`Pending` retains the poller's state and loans; `Ready` transfers the result once.
These are obligations of a Poller implementation and its caller. The ordinary
interface alone does not enforce a terminal state or prevent a later poll.
The future adapter will enforce exclusive polling and trap on a later poll.
The interface can be implemented and driven by ordinary DMM code. The pinned
`fromPoller` adapter and future composition functions are still under
development. `select2`, `race2`, `join2`, `timeout` and nonblocking timers are not
available. Constructing `Context` from an active scheduler task remains
private until the adapter can establish its ownership and cancellation
invariants.

The `language_gaps_async` fixture checks the current Poll/Poller foundation,
no-op contexts and Future payload ownership at O0/O1. It does not establish
adapter pinning, composition wake races or confirmed cancellation of composition
children. See [test coverage](../../tests/stdlib/README.md).
