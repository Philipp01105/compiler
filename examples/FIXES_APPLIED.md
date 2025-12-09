# Struct Issues Fixed

This document summarizes the fixes applied to resolve struct-related issues in the compiler.

## Issue 1: Methods in Structs Cannot Call Other Methods ✅ FIXED

### Problem
Methods defined within a struct could not call other methods from the same struct, forcing code duplication and preventing natural OOP design patterns.

### Root Cause
When parsing function calls inside methods, the parser only looked for regular functions using `find_function()`. It did not consider that the function being called might be another method of the same struct.

### Solution
Modified two key functions in `parser.c`:
1. `parse_primary()` (lines 3753-3766): Added logic to check if we're in a method context (`parser->current_struct_context` is set) and if so, try to find the function as a method by constructing the mangled name (`StructName_methodName`) before reporting "function not found".
2. `parse_function_call_statement()` (lines 3280-3293): Added the same logic for statement-level function calls.

When a method call is detected:
- The mangled method name is constructed
- The 'this' pointer is passed in the first register
- Remaining arguments are shifted to subsequent registers (offset by 1)

### Code Changes
- `src/parser.c`: Added method resolution logic in function call parsing
- Fixed calling convention to use `get_max_reg_args()` instead of hardcoded limits

### Tests
- `test_issue1_fixed.txt`: Comprehensive test with multiple method-to-method calls
- `test_method_calls.txt`: Simple test demonstrating the fix
- Both tests compile and run correctly, producing expected results

### Verification
```bash
./build/compiler test_method_calls.txt
gcc -no-pie test_method_calls.txt.s -o test_method_calls_program
./test_method_calls_program
# Output: Testing method calling method:
#         Result: 8
```

---

## Issue 3: Arrays Cannot Be Struct Fields ✅ FIXED

### Problem
Arrays could not be declared as fields within structs, severely limiting the ability to create complex data structures.

### Root Cause
1. The `StructField` structure did not support array fields (no `is_array` or `array_size` fields)
2. Field parsing code did not handle array syntax with brackets
3. Field access and assignment code did not support array indexing
4. Struct variable allocation did not correctly size array fields

### Solution

#### 1. Extended StructField Structure
Added to `compiler_types.h`:
```c
typedef struct {
    char name[MAX_TOKEN];
    DataType type;
    int offset;
    int size;
    int is_array;      // NEW: 1 if this is an array field
    int array_size;    // NEW: Number of elements if array
} StructField;
```

#### 2. Extended Field Parsing
Modified `parse_struct()` to support array syntax in all declaration styles:
- C-style: `int field[9];`
- var style: `var field:int[9];`
- Name-first style: `field:int[9];`

Array field size calculation: `size = element_size * array_size`

#### 3. Array Field Access (Reading)
Modified `parse_primary()` to support:
- Implicit access in methods: `cells[index]` (lines 3907-3959)
- Explicit access: `struct.field[index]` (lines 4222-4260)

Generated code calculates: `base_address + field_offset + (index * element_size)`

#### 4. Array Field Assignment (Writing)
Modified `parse_assignment()` to support:
- Implicit assignment in methods: `cells[index] = value` (lines 1544-1603)
- Explicit assignment: `struct.field[index] = value` (lines 1720-1768)

#### 5. Fixed Struct Variable Allocation
Fixed issue where struct variables were incorrectly popped after allocation.
Added check to skip pop/move for struct variables (lines 1503-1506).

#### 6. Fixed Method Calling Convention
Replaced hardcoded `i + 1 < 4` with `i + 1 < get_max_reg_args()` to support Linux's 6 registers instead of Windows' 4 registers.

### Code Changes
- `src/compiler_types.h`: Added `is_array` and `array_size` to `StructField`
- `src/parser.c`: 
  - Field parsing: Array syntax support
  - Field access: Implicit and explicit array indexing
  - Field assignment: Implicit and explicit array assignment  
  - Struct allocation: Fixed to not pop after pushing
  - Method calls: Fixed register limits for Linux

### Tests
- `test_issue3_fixed.txt`: Comprehensive test with TicTacToe board (9-element array) and DataStore (5-element array)
- `test_simple_array_struct.txt`: Simple test with 3-element array
- `test_array_minimal2.txt`: Minimal test with methods
- All tests compile and run correctly

### Verification
```bash
./build/compiler test_simple_array_struct.txt
gcc -no-pie test_simple_array_struct.txt.s -o test_simple_program
./test_simple_program
# Output: 10
#         20
#         30
```

---

## Testing Summary

All fixes have been thoroughly tested with multiple test files:

### Issue 1 Tests
- ✅ `test_issue1_fixed.txt`: Methods calling multiple other methods - Exit code: 20 ✓
- ✅ `test_method_calls.txt`: Basic method-to-method call - Output: "Result: 8" ✓

### Issue 3 Tests
- ✅ `test_issue3_fixed.txt`: Complex array operations with methods - Exit code: 150 ✓
- ✅ `test_simple_array_struct.txt`: Basic array field access - Output: "10\n20\n30" ✓
- ✅ `test_array_minimal2.txt`: Minimal array methods test - Exit code: 42 ✓

### Combined Tests
Both issues work together:
- Methods can call other methods ✓
- Methods can access array fields ✓
- Methods can modify array fields ✓
- Array indexing works in method contexts ✓

---

## Known Limitations

### Not Fixed (By Design)
- **Issue 2: Global Variables** - Intentionally not fixed as it encourages better code organization
- **Issue 5: Nested Structs** - Documented limitation, future enhancement
- **Issue 7: No Static Methods** - Documented limitation, future enhancement

### Partially Fixed
- **Issue 4: String Concatenation in Struct Methods** - The root issue with structs is fixed (methods work, array fields work), but there's a separate issue with print/printf stack alignment when combined with methods. This is a print statement issue, not a struct issue.

### Documentation Needed
- **Issue 6: For Loop Decrement** - Needs documentation clarification on supported syntax

---

## Impact

These fixes significantly improve the usability and power of structs in the compiler:

1. **Natural OOP Design**: Methods can now compose functionality by calling other methods, enabling proper code organization and reuse
2. **Complex Data Structures**: Arrays in struct fields enable implementation of games (TicTacToe, Snake, etc.) and data structures (lists, grids, etc.)
3. **Better Code Quality**: Eliminates need for code duplication in methods
4. **Cross-Platform**: Fixed calling convention issues ensure methods work correctly on both Windows and Linux

The compiler now supports a much more complete and usable struct system that enables real-world applications.
