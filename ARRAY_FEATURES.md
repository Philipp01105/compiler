# Array Support - Version 4.5.0

## Overview

This document describes the array support features implemented in compiler version 4.5.0.

## Features

### 1. Array Declaration

Arrays are declared using the `var[]` syntax with explicit type specification:

```c
var[5] numbers:int;      // Array of 5 integers
var[10] bytes:byte;      // Array of 10 bytes
var[3] chars:char;       // Array of 3 characters
```

**Important Notes:**
- Arrays **must** have an explicit type (e.g., `:int`, `:byte`)
- Size specification is required for local arrays
- Arrays are allocated on the stack
- Total size = element_size × array_count

### 2. Array Access

Arrays use zero-based indexing with the `[]` operator:

```c
// Reading
var x:int = numbers[0];
var y:int = numbers[2];

// Writing
numbers[0] = 42;
numbers[1] = x + y;
```

**Supported in:**
- Variable assignment
- Expressions
- Function arguments
- Print statements

### 3. Array Indexing with Expressions

Index can be any integer expression:

```c
var i:int = 2;
numbers[i] = 10;
numbers[i + 1] = 20;
numbers[i * 2] = 30;
```

### 4. Arrays in Loops

Arrays work seamlessly with for-loops:

```c
var[10] arr:int;

// Fill array
for (var i:int = 0; i < 10; i++) {
    arr[i] = i * 2;
}

// Sum elements
var sum:int = 0;
for (var i:int = 0; i < 10; i++) {
    var temp:int = sum + arr[i];
    sum = temp;
}
```

### 5. Supported Data Types

Arrays support all primitive types:

| Type   | Size | Example |
|--------|------|---------|
| `int`  | 4 bytes | `var[5] nums:int;` |
| `byte` | 1 byte | `var[10] data:byte;` |
| `char` | 1 byte | `var[5] text:char;` |
| `bit`  | 1 byte | `var[8] flags:bit;` |

**Note:** Float and double arrays are supported in the type system but not fully tested yet.

### 6. String Indexing

Strings can be indexed to access individual characters:

#### String Variables
```c
var str:string = "Hello";
var ch:char = str[0];    // 'H'
var ch2:char = str[1];   // 'e'
```

#### String Literals
```c
var first:char = "World"[0];   // 'W'
var second:char = "World"[1];  // 'o'
```

### 7. Array Parameters (Pass by Reference)

Arrays can be passed to functions as parameters:

```c
func fillArray(arr[]:int, size:int) -> void
{
    for (var i:int = 0; i < size; i++) {
        arr[i] = 0;
    }
}

func main() -> void
{
    var[10] numbers:int;
    fillArray(numbers, 10);  // Pass array to function
}
```

**How it works:**
- Arrays are passed as pointers (8 bytes)
- Changes to the array inside the function affect the original
- Parameter syntax: `arr[]:type` (no size specified)
- Can pass any size array to the same function

### 8. Array Return Types

Functions can declare array return types:

```c
func getArray() -> int[]
{
    // Implementation...
}
```

**Note:** Currently supported in syntax, but returning arrays requires additional memory management considerations.

### 9. Print Support

Array elements can be printed directly:

```c
var[5] nums:int;
nums[0] = 42;

print("Value: " + nums[0]);  // Prints: Value: 42

// In loops
for (var i:int = 0; i < 5; i++) {
    print("nums[" + i + "] = " + nums[i]);
}
```

## Implementation Details

### Assembly Generation

#### Array Declaration
```c
var[5] numbers:int;  // 20 bytes (5 × 4)
```

Allocates space on stack. No initialization code generated.

#### Array Access (Read)
```c
var x:int = numbers[2];
```

Generated assembly:
```asm
movl $2, %eax              # index
leaq -20(%rbp), %rbx       # base address
movl (%rbx, %rax, 4), %eax # load: base + index * 4
pushq %rax
```

#### Array Assignment (Write)
```c
numbers[2] = 42;
```

Generated assembly:
```asm
movl $42, %eax             # value
pushq %rax
movl $2, %eax              # index
pushq %rax
popq %rcx                  # value
popq %rax                  # index
leaq -20(%rbp), %rbx       # base address
movl %ecx, (%rbx, %rax, 4) # store: base + index * 4
```

#### Array Parameters

Parameters are passed as pointers:
```c
func process(arr[]:int, size:int) -> void
```

Inside function:
```asm
movq -8(%rbp), %rbx        # load array pointer
movl (%rbx, %rax, 4), %eax # access element
```

### Scaled Indexing

The compiler uses x86-64 scaled indexing mode for efficient array access:

- `(%rbx, %rax, scale)` where scale = element_size
- Scale can be 1, 2, 4, or 8

| Type | Scale |
|------|-------|
| byte, char, bit | 1 |
| int | 4 |
| double, string | 8 |

## Examples

### Example 1: Basic Array Operations

```c
func main() -> void
{
    var[5] numbers:int;
    
    // Fill array
    numbers[0] = 10;
    numbers[1] = 20;
    numbers[2] = 30;
    numbers[3] = 40;
    numbers[4] = 50;
    
    // Read and print
    for (var i:int = 0; i < 5; i++) {
        print("numbers[" + i + "] = " + numbers[i]);
    }
}
```

### Example 2: Sum Array Function

```c
func sumArray(arr[]:int, size:int) -> int
{
    var sum:int = 0;
    for (var i:int = 0; i < size; i++) {
        var temp:int = sum + arr[i];
        sum = temp;
    }
    return sum;
}

func main() -> void
{
    var[5] nums:int;
    nums[0] = 10;
    nums[1] = 20;
    nums[2] = 30;
    nums[3] = 40;
    nums[4] = 50;
    
    var total:int = sumArray(nums, 5);
    print("Sum: " + total);  // Prints: Sum: 150
}
```

### Example 3: Copy Array

```c
func copyArray(src[]:int, dst[]:int, size:int) -> void
{
    for (var i:int = 0; i < size; i++) {
        dst[i] = src[i];
    }
}
```

### Example 4: String Indexing

```c
func main() -> void
{
    var str:string = "Hello";
    
    // Access individual characters
    var ch1:char = str[0];  // 'H'
    var ch2:char = str[1];  // 'e'
    
    print("First: " + ch1);
    print("Second: " + ch2);
    
    // String literal indexing
    var w:char = "World"[0];  // 'W'
    print("First of World: " + w);
}
```

## Limitations

1. **No array initialization syntax**: Cannot initialize arrays like `var[3] nums:int = {1, 2, 3};`
2. **No bounds checking**: Index out of bounds will cause undefined behavior
3. **Fixed size**: Array size must be known at compile time
4. **No multi-dimensional arrays**: No support for `var[3][3] matrix:int;` yet
5. **String arrays**: No support for `var[5] strings:string;` (array of strings)

## Test Files

Three comprehensive test files are provided:

1. **array_test_simple.txt** - Basic array operations
2. **array_test.txt** - Comprehensive feature tests
3. **array_functions_test.txt** - Array parameters and functions

Run tests:
```bash
./build/compiler array_test.txt
./build/compiler array_functions_test.txt
```

## Version History

- **4.5.0** - Initial array support implementation
  - Array declaration with size
  - Array element access and assignment
  - String indexing
  - Array parameters (pass by reference)
  - Array return types syntax
  - Print support for array elements

## Future Enhancements

Potential future additions:
- Multi-dimensional arrays
- Dynamic array sizing
- Array initialization syntax
- Bounds checking (debug mode)
- Array of strings support
- Memory-safe array handling
