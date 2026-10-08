# Time

Import "stdlib/time". Duration stores u64 nanoseconds; nanoseconds(value),
milliseconds(value), seconds(value), add, wholeSeconds and subsecondNanos provide checked
construction/arithmetic. now() returns a monotonic Instant; later.since(earlier)
returns Result<Duration,Error>. systemNow() returns Unix seconds and
subsecond nanoseconds; wall time may jump. Precision and range are OS-dependent.

sleep(duration) blocks the calling thread, rounds to milliseconds on Windows,
and retries interrupted nanosleep on Linux. It is not an async executor timer.
Calls use ordinary QueryPerformanceCounter/FileTime or clock_gettime FFI.

Deadline is shared with net and async. noTimeout() denotes infinity; at(ticks) uses absolute monotonic nanoseconds, after(nanoseconds) saturates at the latest finite value. Construction through after follows the fatal clock-failure path; now() exposes recoverable clock errors.

The free after(Duration) constructor returns Result<Deadline,Error>, preserving recoverable clock failures and rejecting overflow into the infinity sentinel. Deadline.after(nanoseconds) is the explicitly saturating constructor and uses the fatal clock-failure path.
