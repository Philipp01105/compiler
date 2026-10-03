# Bytes

Import `"stdlib/bytes"` for unvalidated byte-slice operations. startsWith,
endsWith, find, trimAscii and split do not allocate. find returns Option<usize>.
split(&descriptor,delimiter) returns a lifetime-checked Split borrowing the
byte descriptor. It preserves empty/trailing fields and implements iteration.
Views borrow the backing storage; ASCII trim uses ASCII whitespace only.

clone and replace explicitly allocate collections.List<u8> and return Result
with core.AllocError. An empty replacement needle leaves input unchanged.
UTF-8 validation and owned strings belong in stdlib/text.
