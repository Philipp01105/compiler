# Filesystem

Import "stdlib/fs". Byte paths reject embedded NULs. Windows requires valid
UTF-8, converts strictly to UTF-16 and calls wide OS APIs. Linux preserves native
path bytes through libc OS bindings.
fs.dmm defines the shared public API. Target files provide private native
operations and DirectoryState; users import only this public package.
open(path,Read/Write/ReadWrite/Append) returns Result<File,FsError>. Write
creates/truncates, ReadWrite creates without truncation, Append creates and appends.
File owns its handle, closes in its destructor, and supports idempotent close,
isOpen, read(&mut bytes), write(&bytes), writeAll and seek(offset,Start/Current/End).
Reads/writes may transfer fewer bytes; read returns zero at EOF. writeAll can
fail after partial output. close reports OS failures; destructor close is best effort.

readFile(path,maximum) detects excess input instead of silently truncating;
writeFile, metadata, createDirectory, removeDirectory, removeFile and rename
return Result. Directory creation is single-level. Rename replaces an existing
destination subject to OS rules. Metadata includes kind, size and modification
time; Linux statx does not follow the final symlink, Windows reports reparse points.
Linux permissions use the process umask.

readDirectory returns an owning Directory. next yields an owning DirectoryEntry
with name Bytes and kind, skipping dot entries. EndOfDirectory is an explicit
error variant; entry ordering and availability of kind are platform-dependent.
Directory.close and destruction release its native iterator.
FsError retains domain and native error code. These synchronous operations block.
