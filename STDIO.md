# Streams and buffered I/O

`import "stdlib/stdio";` provides byte-oriented I/O implemented in DMM over
`stdlib/core`. The compiler runtime only supplies OS open/read/write/close and allocation primitives. Streams,
buffering, complete transfers, line handling, integer output, copying, dynamic buffers and error policy are library
code.

```dmm
package main;
import "stdlib/stdio";

func main() -> int {
    var file=stdio.openWrite("example.txt");
    if (!file.isOpen()) { return 1; }
    var writer=stdio.bufferedWriter(&file,4096);
    if (!writer.isOpen()) { file.close(); return 2; }
    var written=writer.writeLine("Hello, streams!");
    var flushed=writer.close();
    if (!flushed.ok()) { writer.discard(); }
    var closed:isize=file.close();
    if (!written.ok() || !flushed.ok() || closed < 0) { return 3; }

    var input=stdio.openRead("example.txt");
    if (!input.isOpen()) { return 4; }
    var line=stdio.readLine(&input,65536);
    var output=stdio.stdout();
    var result=output.writeAll(line.view());
    line.release(); input.close();
    if (!result.ok()) { return 5; }
    return 0;
}
```

## Streams

| API                                          | Behavior                                                                                         |
|----------------------------------------------|--------------------------------------------------------------------------------------------------|
| `openRead(path)`                             | Open an existing file for reading.                                                               |
| `openWrite(path)`                            | Create or truncate a file for writing.                                                           |
| `openAppend(path)`                           | Create if missing; writes append to the existing contents.                                       |
| `openReadWrite(path)`                        | Create if missing, without truncating; read and write share the current position.                |
| `stdin()`, `stdout()`, `stderr()`            | Borrow standard descriptors.                                                                     |
| `borrow(fd, readable, writable)`             | Borrow an external descriptor; the flags describe caller-provided capabilities.                  |
| `Stream.isOpen()`, `canRead()`, `canWrite()` | Inspect wrapper state and declared capabilities.                                                 |
| `Stream.lastError()`                         | Last negative OS error, including a failed open.                                                 |
| `Stream.descriptor()`                        | Descriptor while open, otherwise -1.                                                             |
| `Stream.close()`                             | Close an owned descriptor or detach a borrowed one; return 0 or a negative OS error. Idempotent. |

Failed opens return a closed `Stream`. A default-initialized stream is closed. File creation uses Linux mode 0600
(subject to umask); Windows ignores mode bits. Closing a standard-stream wrapper does not close the OS standard
descriptor. A failed close invalidates the wrapper too: retrying a reused descriptor is unsafe.

Owners must not be copied: DMM currently copies structs and does not enforce move-only resources. Close each owning
stream exactly once, and keep streams alive and at a stable address while buffered adapters borrow them. Do not close or
reuse descriptors behind a live wrapper. There are no implicit destructors.

## Transfers and errors

`read(destination:u8[])`, `write(source:u8[])`, `readExact(destination:u8[])`,
`writeAll(source:u8[])`, `writeString(text:string)` and `writeLine(text:string)`
return `Transfer { count:usize, status:Status, error:isize }`. `ok()` tests for
`Status.Ok`. `count` preserves partial progress on failure. Closed wrappers and disallowed directions fail before
touching buffers; open zero-length transfers succeed without OS I/O. `writeLine` adds LF.

Single reads and writes permit short transfers. Complete-transfer helpers loop until the requested bytes are transferred
or a failure/EOF occurs. Each OS operation is capped at 1 GiB for portability. Zero-progress writes return
`NoProgress` rather than spinning. EOF returns `EndOfStream`; `readExact` also reports how many bytes arrived before
EOF. `readByte()` returns
`ByteResult { value:u8, status:Status, error:isize }`; its value is valid only for
`Status.Ok`.

Statuses are `Ok`, `EndOfStream`, `Closed`, `AccessDenied`, `NoProgress`,
`SystemError`, `OutOfMemory`, `LimitReached`, and `InvalidInput`. OS errors retain their negative platform-dependent
value in `error`; other statuses use zero. There are no automatic retries after OS errors, including interruptions or
nonblocking would-block errors. Standard streams and file streams are synchronous. Strings borrow NUL-terminated
storage; use byte slices for embedded NULs and binary I/O.

`copy(source:*Stream, destination:*Stream, scratch:u8[])` copies until EOF using borrowed, nonempty scratch storage and
reports bytes written, including partial progress before an error. `writeInt(stream, value:i64)` and
`writeUint(stream, value:u64)` format decimal integers in DMM without allocating, including signed minimum and unsigned
maximum.

## Buffered adapters

`bufferedReader(stream:*Stream, capacity:usize)` and
`bufferedWriter(stream:*Stream, capacity:usize)` own heap buffers and borrow the stream. Check `isOpen()` and
constructor `status`; zero capacity or a null stream is invalid, wrong direction is denied, and allocation failure is
reported.

The reader supports `read`, `readExact`, `readByte`, and `readLine(maximum)`. It retains read-ahead between operations.
Its `close()` frees only its buffer; the stream remains the caller's responsibility. Do not mix raw and buffered reads
or multiple readers on the same descriptor; closing a reader discards read-ahead.

The writer supports `write`, `writeString`, `writeLine`, `bufferedBytes`, `flush`,
`close`, and `discard`. Writes count bytes accepted into the buffer; flush counts bytes actually sent to the underlying
stream. Flush preserves an unwritten suffix after partial failure. Successful close flushes and frees only the buffer.
Failed close retains the buffer and pending bytes, allowing retry or explicit `discard()`. Discard releases the buffer
without writing. Flush does not promise disk durability. Do not mix raw writes with pending buffered output. Adapters
must not be copied.

## Owned byte results and files

`readToEnd(stream:*Stream, maximum:usize)`, `readLine(stream:*Stream, maximum:usize)`,
`BufferedReader.readLine(maximum)` and `readFile(path, maximum)` return an owned
`Bytes` collection with `length`, `status`, and `error`. `view()` returns a borrowed slice; `release()` frees storage,
including partial/error results, and is idempotent on the same owner. Do not copy the owner or release its borrowed
view.

Limits bound the returned bytes. `LimitReached` consumes no bytes past the limit and does not imply EOF, even if the
file length exactly equals the limit. Zero limits return `LimitReached` immediately. Dynamic storage grows
geometrically. Read-to-end completion reports `EndOfStream`. Line reads consume and omit LF and the CR immediately
before it; an EOF-terminated final line retains its bytes with
`EndOfStream`. Limits can split a line before its delimiter. Binary NUL bytes survive.

`writeFile(path, source:u8[])` writes all bytes and closes the file, reporting transfer progress and any close error.
`readFile` always closes its stream and returns the byte owner to the caller.

The older `stdlib` descriptor/file helpers remain compatible and now use core I/O and streams. `read_file` validates
capacity against the supplied slice length;
`write_str_to` completes short writes. The compatibility `Input` scanner and floating/string conversion routines retain
their existing runtime implementations.
