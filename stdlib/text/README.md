# Text

Import `"stdlib/text"`. Primitive string operations length, startsWith, endsWith,
find and contains use byte offsets and do not allocate. bytes(string) uses the
runtime string length, preserving embedded NULs.

String owns validated UTF-8. fromString/fromUtf8 explicitly copy and return
Result<String,Error>. builder() creates an allocation-free StringBuilder;
append(string), appendText and appendCodepoint may allocate and return Result.
finish consumes the builder and transfers its storage into String. clear retains
capacity. No writable byte view can invalidate a String's UTF-8 invariant.

String.view() returns a checked Text borrowing its owner. text.view(&bytes)
validates UTF-8 and borrows a checked byte descriptor. Text provides length,
byteAt, substring, startsWith/endsWith/find/contains, copyBytes, codepoints and
splitScalar. Substring requires UTF-8 boundaries. Searches accept literals or
another Text without conversion allocation. Codepoint and split iteration
preserve embedded NULs and trailing empty fields. Offsets and lengths are bytes.
A Text must not outlive its owner or descriptor. Primitive string operations are
free functions; there is no untracked primitive-string-to-Text overload.

text.map<V>() and text.set() provide collections with owned String keys,
borrowed equality and per-owner keyed hashing. Use stdlib/text/utf8 and
stdlib/text/utf16 for compact codecs, stdlib/bytes for unvalidated byte search,
ASCII trimming, splitting, cloning and replacement. Unicode classification,
casing and normalization live in stdlib/unicode and reuse these small codecs.
