# Mini-Grep

```sh
./build/compiler examples/mini_grep/mini_grep.dmm -o build/mini_grep
./build/mini_grep error server.log --ignore-case
./build/mini_grep DMM examples/README.md
printf 'first\nERROR: unavailable\n' | ./build/mini_grep error - --ignore-case
```

The CLI prints matching lines prefixed by their one-based line number. It searches
literal byte substrings; `--ignore-case` folds ASCII A–Z only. An empty pattern
matches every line. `-` selects stdin. Output preserves input bytes within each
line, including CR in CRLF files, and ends each emitted line with LF.

The generic `search<R:io.Reader,W:io.Writer>` reads 4096-byte chunks and retains
only the current line, so matches can cross chunk boundaries. Pattern and path
limits are 4096 bytes; a line is limited to 1 MiB. The final unterminated line is
also searched. Files and line storage are released through ownership.

Exit codes: 0 means at least one match, 1 means no matches, 2 means invalid
arguments or an I/O/resource error. There are no regular expressions or recursive
directory searches. On Windows, use an `.exe` output name.
