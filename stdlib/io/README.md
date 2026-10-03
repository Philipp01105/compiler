# I/O

Import `"stdlib/io"`. Reader and Writer have mutating read(&mut u8[]) and
write(&u8[]) methods returning Result<usize,Error>. Short transfers are normal;
read returns zero at EOF. Error records kind, native domain/code and transferred
bytes. A Writer failure must report the bytes accepted by that call.

stdin/stdout/stderr return standard-handle adapters. print/println return Result.
readExact rejects early EOF; writeAll rejects zero progress. readAll owns its
returned List<u8>; readToEnd appends to a caller's List. Both support size limits.
copy uses fixed scratch storage. Failed readAll destroys unpublished output;
readToEnd preserves bytes already appended and reports transfer progress.

reader(&bytes) and writer(&mut bytes) are checked memory adapters.
bufferedReader(&mut source,capacity) and bufferedWriter(&mut target,capacity)
allocate owned buffering, borrow the resource and reject zero capacity.
BufferedReader.readLine(maximum) returns Result<Option<List<u8>>,Error>, strips
LF/CRLF, preserves trailing unterminated input and returns None at EOF.
BufferedWriter.flush retains unsent data after a short/error transfer so it can
be retried. pending reports unsent bytes. Flush explicitly before destruction:
the destructor releases storage and does not conceal a fallible output operation.

Files implement the same protocols. Standard native operations handle EINTR on
Linux and chunk native transfers on Windows. These APIs are synchronous. Numeric
printing still uses existing runtime formatting; remaining formatting migration
is tracked in plans/stdlib-redesign-implementation.md.
