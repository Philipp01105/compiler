# DMM Standard Library

A minimal, foundational standard library for the DMM programming language.

## Philosophy

This standard library provides **essential primitives** and **fundamental operations** that:
- Cannot be easily implemented in pure DMM code
- Are commonly needed across many programs
- Provide a solid foundation for higher-level abstractions

The library is intentionally minimal and focused on core functionality rather than convenience wrappers.

## Modules

### core.dmm - Core Primitives

Essential operations on primitive data types.

**Integer Operations:**
- `abs(n:int) -> int` - Absolute value
- `min(a:int, b:int) -> int` - Minimum of two integers
- `max(a:int, b:int) -> int` - Maximum of two integers
- `clamp(value:int, minVal:int, maxVal:int) -> int` - Clamp value to range
- `sign(n:int) -> int` - Sign of integer (-1, 0, or 1)
- `isEven(n:int) -> bit` - Check if even
- `isOdd(n:int) -> bit` - Check if odd

**Boolean Operations:**
- `not(b:bit) -> bit` - Logical NOT
- `and(a:bit, b:bit) -> bit` - Logical AND
- `or(a:bit, b:bit) -> bit` - Logical OR
- `xor(a:bit, b:bit) -> bit` - Logical XOR

**Character Operations:**
- `isUpper(c:char) -> bit` - Check if uppercase letter
- `isLower(c:char) -> bit` - Check if lowercase letter
- `isAlpha(c:char) -> bit` - Check if alphabetic
- `isDigit(c:char) -> bit` - Check if digit
- `isAlnum(c:char) -> bit` - Check if alphanumeric
- `isSpace(c:char) -> bit` - Check if whitespace
- `toLower(c:char) -> char` - Convert to lowercase
- `toUpper(c:char) -> char` - Convert to uppercase

**Utilities:**
- `swap(arr[]:int, i:int, j:int) -> void` - Swap array elements

### io.dmm - Input/Output

Simple I/O operations using compiler built-ins.

**Input (wraps built-in scanf functions):**
- `readInt() -> int` - Read integer from stdin
- `readChar() -> char` - Read character from stdin
- `readString() -> string` - Read string from stdin

**Output:**
- `println(s:string) -> void` - Print string with newline
- `printInt(n:int) -> void` - Print integer
- `printlnInt(n:int) -> void` - Print integer with newline
- `printChar(c:char) -> void` - Print character
- `printlnChar(c:char) -> void` - Print character with newline

### math.dmm - Mathematical Operations

Mathematical functions and algorithms.

**Basic Math:**
- `pow(base:int, exponent:int) -> int` - Power function
- `square(n:int) -> int` - Square
- `cube(n:int) -> int` - Cube
- `sqrt(n:int) -> int` - Integer square root (floor)

**Number Theory:**
- `gcd(a:int, b:int) -> int` - Greatest common divisor
- `lcm(a:int, b:int) -> int` - Least common multiple
- `isPrime(n:int) -> bit` - Check if prime
- `factorial(n:int) -> int` - Factorial
- `fibonacci(n:int) -> int` - Fibonacci number

**Summation:**
- `sum(n:int) -> int` - Sum of 1 to n
- `sumRange(start:int, end:int) -> int` - Sum of range

**Digit Operations:**
- `digitCount(n:int) -> int` - Count digits
- `digitSum(n:int) -> int` - Sum of digits
- `digitReverse(n:int) -> int` - Reverse digits

### strings.dmm - String Operations

String analysis and manipulation (strings are immutable).

**Analysis:**
- `length(s:string) -> int` - String length
- `isEmpty(s:string) -> bit` - Check if empty
- `charAt(s:string, index:int) -> char` - Get character at index
- `indexOf(s:string, c:char) -> int` - Find first occurrence
- `lastIndexOf(s:string, c:char) -> int` - Find last occurrence
- `countChar(s:string, c:char) -> int` - Count character occurrences

**Comparison:**
- `compare(s1:string, s2:string) -> int` - Compare strings (-1, 0, 1)
- `equals(s1:string, s2:string) -> bit` - Check equality
- `startsWith(s:string, c:char) -> bit` - Check start character
- `endsWith(s:string, c:char) -> bit` - Check end character

**Classification:**
- `isAlphaStr(s:string) -> bit` - All alphabetic
- `isDigitStr(s:string) -> bit` - All digits
- `isAlnumStr(s:string) -> bit` - All alphanumeric
- `isUpperStr(s:string) -> bit` - All uppercase
- `isLowerStr(s:string) -> bit` - All lowercase

**Conversion:**
- `charToDigit(c:char) -> int` - Character to digit value
- `digitToChar(digit:int) -> char` - Digit to character
- `parseInt(s:string) -> int` - Parse integer from string

**Utilities:**
- `countWords(s:string) -> int` - Count words
- `countLines(s:string) -> int` - Count lines

### array.dmm - Array Operations

Array manipulation and analysis.

**Initialization:**
- `fillInt(arr[]:int, size:int, value:int) -> void`
- `fillByte(arr[]:byte, size:int, value:byte) -> void`
- `fillChar(arr[]:char, size:int, value:char) -> void`

**Search:**
- `indexOfInt(arr[]:int, size:int, value:int) -> int`
- `containsInt(arr[]:int, size:int, value:int) -> bit`
- `countInt(arr[]:int, size:int, value:int) -> int`

**Statistics:**
- `minInt(arr[]:int, size:int) -> int`
- `maxInt(arr[]:int, size:int) -> int`
- `sumInt(arr[]:int, size:int) -> int`
- `averageInt(arr[]:int, size:int) -> int`
- `indexOfMinInt(arr[]:int, size:int) -> int`
- `indexOfMaxInt(arr[]:int, size:int) -> int`

**Manipulation:**
- `reverseInt(arr[]:int, size:int) -> void`
- `copyInt(source[]:int, dest[]:int, size:int) -> void`
- `multiplyScalarInt(arr[]:int, size:int, scalar:int) -> void`
- `addScalarInt(arr[]:int, size:int, scalar:int) -> void`

**Sorting:**
- `sortInt(arr[]:int, size:int) -> void` - Bubble sort
- `isSortedInt(arr[]:int, size:int) -> bit`

**Comparison:**
- `equalsInt(arr1[]:int, arr2[]:int, size:int) -> bit`

**Printing:**
- `printArrayInt(arr[]:int, size:int) -> void`
- `printlnArrayInt(arr[]:int, size:int) -> void`

## Usage

Import modules as needed:

```javascript
#import <stdlib/core.dmm>
#import <stdlib/io.dmm>
#import <stdlib/math.dmm>
#import <stdlib/strings.dmm>
#import <stdlib/array.dmm>
```

## Examples

### Example 1: Basic I/O
```javascript
#import <stdlib/io.dmm>

func main() -> void {
    println("Enter a number:");
    var n:int = readInt();
    println("You entered:");
    printlnInt(n);
}
```

### Example 2: Math Operations
```javascript
#import <stdlib/math.dmm>
#import <stdlib/io.dmm>

func main() -> void {
    var n:int = 10;
    var fib:int = fibonacci(n);
    var fact:int = factorial(n);
    
    println("Fibonacci(10):");
    printlnInt(fib);
    println("Factorial(10):");
    printlnInt(fact);
}
```

### Example 3: String Analysis
```javascript
#import <stdlib/strings.dmm>
#import <stdlib/io.dmm>

func main() -> void {
    var text:string = "Hello World";
    var len:int = length(text);
    var words:int = countWords(text);
    
    println("Length:");
    printlnInt(len);
    println("Words:");
    printlnInt(words);
}
```

### Example 4: Array Operations
```javascript
#import <stdlib/array.dmm>
#import <stdlib/io.dmm>

func main() -> void {
    var[5] numbers:int;
    numbers[0] = 5;
    numbers[1] = 2;
    numbers[2] = 8;
    numbers[3] = 1;
    numbers[4] = 9;
    
    println("Before sort:");
    printlnArrayInt(numbers, 5);
    
    sortInt(numbers, 5);
    
    println("After sort:");
    printlnArrayInt(numbers, 5);
}
```

## Design Notes

- **Minimal Dependencies**: Each module imports only what it needs
- **Explicit Sizes**: Array functions require explicit size parameters (no implicit length)
- **Type Specific**: Functions are typed (e.g., `fillInt`, `fillChar`) for type safety
- **Immutable Strings**: String operations work with immutable string literals
- **No Dynamic Allocation**: All operations work with stack-allocated or static data
- **Integer Math Only**: No floating-point operations (use integer approximations)

## Built-in Compiler Functions

The compiler provides these built-in functions that the stdlib wraps:
- `scanfInt() -> int` - Raw integer input
- `scanfChar() -> char` - Raw character input
- `scanfString() -> string` - Raw string input
- `print(x)` - Print any type

## Version

Standard Library Version: 2.0.0  
Compiler Version: 5.1.0+  
Date: November 2025
