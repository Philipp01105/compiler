# Issue 6: For Loop Increment/Decrement Syntax

## Status: ✅ ALREADY WORKING

The compiler fully supports both `i++` and `i--` operators in for loops, as well as compound assignment operators.

## Supported Syntax

### Increment Operators
```c
// Post-increment
for (var i:int = 0; i < 10; i++) {
    // ...
}

// Compound assignment
for (var i:int = 0; i < 10; i += 1) {
    // ...
}

// Explicit assignment
for (var i:int = 0; i < 10; i = i + 1) {
    // ...
}
```

### Decrement Operators
```c
// Post-decrement
for (var i:int = 10; i > 0; i--) {
    // ...
}

// Compound assignment
for (var i:int = 10; i > 0; i -= 1) {
    // ...
}

// Explicit assignment
for (var i:int = 10; i > 0; i = i - 1) {
    // ...
}
```

### Other Compound Operators
```c
// Multiply
for (var i:int = 1; i < 1000; i *= 2) {
    // ...
}

// Divide
for (var i:int = 1000; i > 0; i /= 2) {
    // ...
}
```

## Implementation Details

The for loop increment/decrement operators are implemented in `parse_for_loop()` function (lines 2096-2226 in `src/parser.c`).

Specifically, the increment section is parsed at lines 2147-2200:
- Lines 2161-2170: Handle `TOKEN_PLUS_PLUS` and `TOKEN_MINUS_MINUS`
- Lines 2171-2190: Handle compound assignment operators (`+=`, `-=`, `*=`, `/=`)
- Lines 2191-2196: Handle explicit assignment (`i = expression`)

## Code Generation

For `i++`:
```asm
movl offset(%rbp), %eax
addl $1, %eax
movl %eax, offset(%rbp)
```

For `i--`:
```asm
movl offset(%rbp), %eax
subl $1, %eax
movl %eax, offset(%rbp)
```

## Test

See `test_issue6_forloop.txt` for a comprehensive test covering all syntax variations.

The test verifies:
- ✅ `i++` increment
- ✅ `i--` decrement  
- ✅ `i = i + 1` explicit increment
- ✅ `i = i - 1` explicit decrement
- ✅ All variations work correctly

## Conclusion

**This issue does not need to be fixed** - the functionality is already fully implemented and working. This document serves as confirmation and reference for the supported syntax.
