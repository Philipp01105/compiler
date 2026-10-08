# stdlib/limits

`import "stdlib/limits";` exposes typed compile-time constants for every bounded
scalar primitive. Names are I8_MIN/I8_MAX, through I16/U16/I32/U32/I64/U64,
plus ISIZE/USIZE, INT, CHAR, BYTE, BIT, FLOAT and DOUBLE. Unsigned minimums are zero;
Use true/false directly for bit. DMM char is signed 8-bit, byte unsigned 8-bit and int
signed 32-bit. Pointer-width integers are 64-bit on ELF and COFF targets.

FLOAT_MIN/DOUBLE_MIN denote the most negative finite values; MAX denotes the largest
finite positive value. MIN_NORMAL denotes the smallest positive normal number,
MIN_POSITIVE the smallest positive subnormal number, and EPSILON the gap between 1
and the next representable value. The representations are IEEE binary32/binary64.

Strings, pointers, slices, functions and aggregates have no numeric minimum/maximum;
void and never have no values. This package defines no fabricated bounds for them.
All declarations are ordinary DMM constants, without runtime dependencies.
