# Custom Print Implementation - No C Library

## Overview
This implementation replaces the C library `printf` function with custom assembly functions that use direct system calls. The compiler now generates completely self-contained code for printing without any external library dependencies.

## Implementation Details

### Helper Functions Generated
The compiler generates four helper functions in the assembly output:

1. **`__print_string`** - Prints a null-terminated string
   - Calculates string length by scanning for null terminator
   - Uses Linux syscall `write(1, buffer, length)` (syscall number 1)
   
2. **`__print_int`** - Converts and prints a 32-bit integer
   - Handles negative numbers (prepends '-' sign)
   - Converts integer to ASCII string using division by 10
   - Calls `__print_string` to output the result
   - Properly saves/restores callee-saved register (rbx)
   
3. **`__print_char`** - Prints a single character
   - Creates a 2-byte buffer (char + null terminator) on stack
   - Calls `__print_string` to output
   
4. **`__print_newline`** - Prints a newline character
   - Creates a 2-byte buffer ('\n' + null terminator) on stack
   - Calls `__print_string` to output

### Modifications to Parser
- Added `print_call_count` field to Parser struct to track usage
- Modified `parse_print_statement()` to call `__print_*` functions instead of `printf`
- Modified `parse_printline_statement()` similarly, adding `__print_newline` call
- Replaced format string loading with direct value passing to helper functions

### System Call Interface
```assembly
# Linux syscall write(fd, buffer, length)
mov rdi, 1          # fd = stdout (file descriptor 1)
mov rsi, buffer     # buffer pointer
mov rdx, length     # number of bytes to write
mov rax, 1          # syscall number for write
syscall             # invoke kernel
```

## Usage Examples

### Before (with printf):
```assembly
lea rdi, [rip + .LC_int_format]  # "%d"
mov esi, 42
call printf
```

### After (with custom implementation):
```assembly
mov edi, 42
call __print_int
```

## Compatibility

### ✅ Supported:
- Linux x86-64 (ELF format)
- String literals
- Integer values (positive and negative)
- Character values
- Mixed print/println operations

### ⚠️ Limitations:
- Windows COFF format has placeholder only (would need WriteFile API)
- Complex expressions requiring runtime type conversion (e.g., string concatenation)
- Float/double printing (partially supported)

## Testing

Run the comprehensive test:
```bash
./build/compiler test_custom_print.dmm
gcc -no-pie test_custom_print.dmm.s -o test
./test
```

Expected output:
```
=== Custom Print Test ===

String: Hello!
Integer: 42
Negative: -99

All tests completed successfully!
```

## Verification

To verify no C library functions are called:
```bash
grep -i "call printf" output.s    # Should find nothing
grep "syscall" output.s            # Should find syscall instructions
```

## Performance Notes

- Function call overhead is minimal (same as printf)
- String length calculation is O(n) but necessary for syscall
- Integer to string conversion uses repeated division (acceptable for typical values)
- Stack usage is minimal and properly aligned

## Future Improvements

1. **Inlining Optimization**: If only one print call exists, inline the code instead of generating a function
2. **Windows Support**: Implement proper WriteFile API calls for COFF format
3. **Float Support**: Add proper float-to-string conversion
4. **Type System Integration**: Handle complex expressions with automatic type conversion

## Benefits

1. ✅ No C library dependency for print operations
2. ✅ Smaller binary size (no libc needed for simple programs)
3. ✅ Direct control over output mechanism
4. ✅ Educational value - shows how printing actually works at syscall level
5. ✅ Platform independence (easy to port to different syscall interfaces)
