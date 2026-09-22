# DMM source formatting

Run `python tools/format_dmm.py` from the compiler repository to format its DMM
sources, or append `--check` to verify them without writing. Explicit file paths
limit the operation. `--root ..` also includes the sibling IDE examples and
fixtures in this workspace. Build output and Git internals are excluded.

The common style uses four spaces, one statement per line, multiline block
bodies, spaces around binary operators and after commas/colons, and blank lines
between declarations. Long parameter lists, calls and conditions wrap at roughly
110 columns. A named initializer with more than three attributes always puts
each attribute on its own line; shorter initializers also wrap when necessary.
String and data literals retain their exact contents.

The formatter checks token preservation and idempotence before writing a file.
It supports deliberately malformed diagnostic fixtures, including incomplete
numeric exponents, without repairing their errors. Unicode table generation
uses the same formatter, so regenerating data preserves the source style.
