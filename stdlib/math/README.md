# Mathematics

Import "stdlib/math". PI, TAU, E and double functions abs, sqrt, sin, cos, tan,
log, exp, pow, floor, ceil, trunc and remainder use standard platform math FFI.
remainder has fmod semantics (the dividend determines the sign).
isNaN and isFinite inspect IEEE-754 bits. min/max propagate NaNs; clamp requires
ordered low <= high bounds. Domain and overflow behavior follow native IEEE math.
No locale-sensitive text conversion is performed by this package.

