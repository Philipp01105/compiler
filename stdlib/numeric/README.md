# stdlib/numeric

Checked `u64` and `i64` add/subtract/multiply/divide, strict integer parsing and
allocation-free formatting. All functions are normal DMM code. Narrow integers can be
widened explicitly; callers check the target range before narrowing a result.

`checkedAdd`, `checkedSub`, `checkedMul`, `checkedDiv` return `Result<integer,NumberError>`.
Signed minimum times/divided by -1 reports Overflow, division by zero DivisionByZero.

`parseUint(bytes,base)` and `parseInt(bytes,base)` accept bases 2–36 with ASCII digits
0–9, A–Z, a–z. Signed parsing allows one initial + or -. They require the entire input:
no whitespace, separators, NUL terminators or implicit radix prefixes. Empty input or a
bare sign is Empty. Invalid base/digit and overflow are reported explicitly.

`formatUint(value,base,output)` and `formatInt(value,base,output)` return the number of
bytes written. Output is lowercase ASCII without a terminator, prefix or padding.
Zero emits one digit. Both leave output unchanged on failure. Buffer sizes 64/65 cover
all unsigned/signed 64-bit values in every supported base. Only the returned prefix is
initialized; callers retain ownership of the buffer.
