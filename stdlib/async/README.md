# stdlib/async

Importing this package exposes `asynchronous` and requires the async manifest feature.

## Pollers and contexts

`Waker` retains its target; cloning retains it again and dropping releases it. `Context` owns a waker and can retain a copy. A no-op waker cannot drive a Pending operation that needs a later wake.

`Poller<T>` is a structural interface: `poll(&mut Context)` returns `Poll<T>` and `cancelPoll(&mut Context)` returns `Poll<CancelAck>`. Pending must arrange another wake. ReadyConfirmed means outstanding work and its waking activity have ended. Native state must remain live until that acknowledgement. These are implementation obligations; the interface alone does not enforce native termination.

`fromPoller<T,P>` owns a poller in a pinned async frame, polling and parking on Pending. Cancellation after the body starts runs deferred cancelPoll until acknowledgement before cleanup. Cancellation before the first body execution only cleans up captured parameters, so a poller's initial native state must already be safe to destroy.

## Future handles

`poll` consumes a handle and returns `FuturePoll<T>.Pending(Future<T>)` or `Ready(T)`. A Pending handle retains its captured loans and must still be consumed. `pollVoid` uses `VoidFuturePoll` for Future<void>. An already-ready handle can be consumed through await/block_on without polling it again.

The compiler operations `futurePoll` and `futureCancelPoll` require an exclusive checked reference to a Future. `futureComplete` and `futureCancelComplete` consume a terminal handle after checking its state. `asyncContext` supplies the current raw context only inside async functions. Raw contexts/native frames are a trusted ABI boundary; prefer the Context/Poller adapters. See [LANGUAGE_SPEC.md](../../LANGUAGE_SPEC.md).

## Composition and timers

| Operation | Result and ownership |
|---|---|
| select2(left,right) | Selected.Left(value,rightHandle) or Right(leftHandle,value); polls both inputs |
| race2(left,right) | Winning value, after confirmed cancellation of the loser |
| join2(left,right) | Joined with left/right values |
| timers.sleep(duration) | Future<void> completing after the duration |
| timers.timeout(future,duration) | Timed.Completed(value) or Elapsed, after cancellation acknowledgement |

The remaining handle from select2 must be consumed even if it is already ready. Generic value composition uses a Unit payload for void results; void polling has its own API.

The Timer poller returns Unit. Its worker sleeps in chunks of at most one millisecond, checks cancellation and wakes the retained registered context. After acknowledgement, cleanup joins the worker and frees its state. Dropping a started Timer before completion/cancellation acknowledgement traps; use fromPoller, sleep or timeout to manage this lifecycle.

## Validation status

The language_gaps_async fixture covers typed polling, pinned poller state, repeated cancellation acknowledgement, named consuming callables, selection, race, join, sleep and timeout. It also checks nested/reordered Future payloads, independent replacement loans and borrowed aggregate results through block_on, .await() and futureComplete. The current Windows gap contract passed at O0/O1, and a focused Linux O1 execution passed.

The earlier collection compilation regressions are fixed. Full container-loan release and further scheduler/wake race coverage remain open. See [test status](../../tests/stdlib/README.md) and [remaining gaps](../../plans/stdlib-language-gaps.md).
