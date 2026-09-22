# stdlib/binary

Bounded allocation-free binary cursors in DMM. `reader()` and `writer()` own only their
position. Pass a checked source/destination descriptor borrow to each operation; the
cursor does not retain a raw pointer or borrowed reference. Reuse the same logical
buffer for a cursor, or construct a new cursor when switching buffers.

The current dev edition also provides `borrowedReader(&bytes)` and
`borrowedWriter(&mut bytes)`. These adapters retain a checked borrow and omit the
buffer argument on each operation. The source descriptor and its backing storage
must stay live. A BorrowedReader can be copied with an independent cursor;
BorrowedWriter moves and preserves exclusive access. Neither adapter can be
default-initialized. Their bounds checks and error results are those of the cursors.

```dmm
package main;
import (
    "stdlib"
    "stdlib/binary"
);
func main() -> int {
    var storage:u8[4]; var bytes:u8[]=storage;
    var output=binary.writer();
    var result=output.writeUint(&mut bytes,16909060,4,binary.Endian.Big);
    match(result) { Ok => {} Err(error) => return 1; }
    var input=binary.reader();
    var decoded=input.readUint(&bytes,4,binary.Endian.Big);
    match(decoded) { Ok(value) => { if(value!=16909060) { return 2; } } Err(error) => return 3; }
    return 0;
}
```

`readUint` / `writeUint` accept widths 1, 2, 4 or 8 bytes and Little/Big endian.
Writer values must fit the selected width. Reader `skip(source,count)` advances without
reading. `readBytes(source,destination)` and `writeBytes(destination,source)` copy the
entire requested region; source and destination storage must be disjoint.

`offset()` reports the current position, `remaining(buffer)` the available length (zero
if the cursor exceeds a replacement buffer). All operations return typed errors, preserve
position on failure and avoid partial output. Invalid widths and overflowing values are
checked before capacity. No signed casts, native alignment assumptions or OS dependencies.
