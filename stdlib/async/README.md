# Async composition

Import "stdlib/async" (package asynchronous), with features = ["async"] in the root manifest. Compiler-owned Future, JoinHandle, Executor, spawn, block_on, cancel and shutdown keep their language spellings and mandatory consumption obligations.

| API | Ownership contract |
|---|---|
| select(left,right) | Polls both; Selected.Left(value,rightHandle) or Right(leftHandle,value) transfers the remaining Future to the caller |
| race(left,right) | Winning value after acknowledged cancellation of the loser |
| join(left,right) | Joined.left/right after both children complete |
| sleep(Duration), sleepUntil(Deadline) | Future<void>; timer cleanup acknowledges cancellation |
| timeout(future,Deadline) | Timed.Completed(value) or Elapsed; cancels and confirms the losing child |
| asUnit(Future<void>) | Future<Unit> for generic value composition |
| selectVoid | Selected<Unit,Unit>; the remaining Future<Unit> must be consumed |
| joinVoid/raceVoid | Consuming Future<void> adapters |
| timeoutVoid | TimedVoid.Completed or Elapsed |

Void adapters are explicit because generic payload composition needs a value type. Every selected remaining handle must be awaited or cancelled, including an already ready handle. Dropping a live Future is forbidden. Loans release only after completion or cancellation acknowledgement.

Manual polling, Waker/Context, Poller and native timer state live in [async/poll](poll/README.md). Context is refreshed on each poll and cancellation poll, so pending handles can move between driving contexts. Normal timers use time.Deadline, shared with networking, and reevaluate the absolute monotonic deadline between worker sleeps.

Future frames and scheduler resources retain the existing fatal allocation contract. Noncooperative synchronous code and missing cancellation acknowledgement can delay cancellation indefinitely. See [the language contract](../../LANGUAGE_SPEC.md).
