# Text

Import "stdlib/text". Operations use UTF-8 byte slices and byte offsets.
bytes(string) borrows a NUL-terminated string up to its first NUL. Byte slices
preserve embedded NULs. startsWith, endsWith, find, trimAscii and split do not
allocate; find returns Option<usize>. The move-only Split.next returns Option<u8[]> and
preserves empty and trailing fields. The delimiter is a single byte.

builder(capacity) returns Result<StringBuilder,stdlib.AllocationError>; append, appendString,
clear, length and view build owned bytes. clone and replace return owned Bytes.
Empty replacement needles leave the input unchanged. Fallible operations return
allocation errors. Views and iterators borrow their backing owner.

Use stdlib/unicode for Unicode validation, classification, casing, normalization
and UTF-16 conversion. ASCII trimming deliberately uses ASCII whitespace.
