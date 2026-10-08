# JSON documents and key lookup

```sh
./build/compiler examples/json_parser/json_parser.dmm -o build/json_parser
./build/json_parser examples/json_parser/sample.json
./build/json_parser examples/json_parser/sample.json service
./build/json_parser examples/json_parser/sample.json ports 0
```

The reusable package returns `core.Result<json.Document,json.Error>` from
`json.parse(&input)`. The document owns decoded strings, keys, booleans and exact
number spellings. It has no references to the input buffer, which can be released
immediately after parsing. Containers retain their children through private
indices; callers use checked values rather than those storage details.

For example, with `import "examples/json_parser/json"` and `"stdlib/core"`:

```dmm
func firstPort(document: &json.Document) -> core.Result<i64, json.AccessError> {
    var root = document.root();
    var ports = root.get("ports")?;
    var value = ports.at(0)?;
    return value.asInt();
}
```

| Operation | Result |
|---|---|
| `document.root()` | Checked `Value` borrowing the document |
| `value.kind()` | Object, Array, String, Number, Boolean or Null |
| `value.get("name")`, `value.get(textView)` | Object value by decoded key |
| `value.at(index)` | Array value by zero-based index |
| `value.length()` | Object member count or array length |
| `value.entry(index)` | Object member with decoded `key` and checked `value` |
| `value.asString()` | Decoded `text.Text` view, including escapes and embedded NUL |
| `value.asBool()` | A DMM `bit` |
| `value.asNumber()` | Exact validated number text |
| `value.asInt()`, `value.asUint()` | Checked i64/u64 integer conversion |

Except for `root` and `kind`, these operations return Result with `AccessError`.
MissingKey, WrongType and OutOfBounds distinguish absent keys, unsuitable value
types and invalid indices. JSON null is an actual value with `Kind.Null`.
Views and values cannot outlive the document or remain live while it is moved.
Lookup does not retain the lookup key. No allocation is needed for access.

Object lookup compares decoded UTF-8 keys, including escaped and non-ASCII keys.
For duplicate keys, the last value wins lookup. Entry enumeration retains all
members in source order, including duplicates. Key lookup is linear in the
object's members; array indexing and entry enumeration traverse child links.

Numbers stay exact, including values outside machine ranges. Integer access
reports Overflow when a value exceeds its range and InvalidNumber for fractions,
exponents or a negative unsigned value. Floating-point conversion is not provided.

The CLI reads up to 1 MiB and displays the whole document or a selected value.
Additional arguments select object keys or array indices; an object key that
looks numeric is still treated as a key. Display quotes decoded strings again
so control characters are visible. The parser validates complete input, JSON
whitespace, escapes and UTF-8 using a bounded explicit stack. Nesting is limited
to 64 containers; unpaired surrogate escapes are rejected.

Exit codes: 0 success, 1 JSON syntax/encoding/allocation error with a byte offset,
2 argument/file/display error, 3 lookup failure. On Windows, use an `.exe` output
name. `json_values_contract` checks ownership after the input is freed, key/index
access, decoding, integer ranges and rejected document lifetime escapes.
