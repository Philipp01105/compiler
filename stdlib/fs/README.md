# Filesystem

Import `"stdlib/fs"` for portable, synchronous filesystem operations.
`open(path)`, `create(path)`, and `openAppend(path)` return
`core.Result<File,fs.Error>`. Explicit OpenMode selects Read, Write,
ReadWrite or Append. Write truncates; ReadWrite creates without truncating.
String convenience paths and checked byte paths reject embedded NULs.
Windows requires valid UTF-8 and uses wide native APIs; Linux preserves path bytes.

File owns its handle and closes on destruction. Explicit close is idempotent
and reports errors. read/write may transfer fewer bytes; read returns zero at EOF.
File implements io.Reader/io.Writer, and read/write/readAll/writeAll/seek/close
use io.Error, including native error domain/code and partial-transfer progress.
Use SeekOrigin.Start/Current/End. Destructor cleanup is best effort.

readFile(path[,maximum]) returns an owned collections.List<u8> and rejects input
exceeding the explicit limit. writeFile writes the supplied borrowed bytes.
metadata, exists, removeFile, rename, createDir and removeDir return Result.
createDirs creates missing parent directories. Path normalization is lexical.
Metadata contains kind, size and modification time. Linux metadata does not
follow the final symlink; Windows reports reparse points. Native permission,
symlink and rename behavior remains platform dependent.

openDir returns an owning Directory. next returns Result<Option<Entry>,Error>:
Ok(None) means EOF. Entries own their name in List<u8>; dot entries are skipped.
readDir collects entries in an owned List. Ordering is platform dependent.
Directory.close/destruction releases the native iterator.

Target-specific files implement private native operations. Normal applications
import fs and io; raw handles and native bindings are separate facilities.
