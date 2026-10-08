# Mutex and guards

Import `"stdlib/sync"`. `sync.mutex(value)` takes ownership and returns
`core.Result<sync.Mutex<T>,core.AllocError>`, requiring `T:core.Send`.
The mutex is Send and Sync. `sync.lock(&mutex)` blocks until it can return an owning
`MutexGuard<'a,T>`. The guard keeps the mutex borrowed until its destructor unlocks.
`guard.get()` and `guard.getMut()` return checked references tied to the guard;
they cannot outlive it. Moving a guard transfers its single unlock responsibility.
Guards are not Send. Locks are nonrecursive: acquiring the same mutex again on the
same thread while holding its guard blocks.

Private target implementations use Windows SRW locks or Linux pthread mutexes.
Applications select neither implementation nor raw OS state. Mutex destruction
destroys the protected value once, releases the OS state and frees its allocation.
Allocation failure destroys the incoming payload. Mutex locking is synchronous;
avoid blocking executor workers on locks held by tasks that need those workers.
