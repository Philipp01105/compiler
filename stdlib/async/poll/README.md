# Advanced polling

Import "stdlib/async/poll" (package polling) with the async feature. Poll<T> is Pending/Ready(T). Context owns a retained Waker; retainWaker()/clone() keep the task and executor alive. wake() schedules progress. noopWaker() and context(waker) support manual drivers; scheduler-context construction stays private.

Poller<T> supplies poll(&mut Context)->Poll<T> and cancelPoll(&mut Context)->Poll<CancelAck>. Ready(Confirmed) acknowledges that child work and wake registrations have ended. fromPoller<T,P> pins and owns P, parks on Pending and polls cancellation to acknowledgement before destruction. Unstarted pollers must be safe to destroy without polling.

poll(Future<T>,&mut Context) returns FuturePoll.Pending(Future<T>) or Ready(T), preserving pending ownership and loans. pollVoid handles Future<void>. The advanced select driver polls both children with the current retained scheduler context. Normal composition lives in stdlib/async.

timer(time.Deadline) constructs a Timer poller yielding Unit. Its worker checks cancellation between waits of at most one millisecond. A started Timer must complete or acknowledge cancellation before destruction; violating that advanced contract traps. Normal callers use async sleep/timeout.
