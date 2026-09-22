# Paths

Import "stdlib/path". Style.Posix and Style.Windows are explicit; nativeStyle()
selects the host target. valid rejects embedded NULs. isAbsolute, basename,
dirname and extension return byte-based results; the latter three borrow input.
Hidden names without another dot have no extension. A basename of a root is empty;
dirname of a single relative component is empty.

join and normalize return owned Bytes in Result. Normalization is lexical:
it removes redundant separators and dot components, resolves parent components
without crossing an absolute root or UNC server/share, and preserves leading
relative parents and drive-relative paths. Empty input normalizes to ".".
Windows accepts slash and backslash and emits backslash. No filesystem lookup,
symlink resolution, case folding or Windows device-path rewriting is performed.

