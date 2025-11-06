# Standard Library Documentation

## Overview

The compiler standard library provides a comprehensive set of functions for common programming tasks. The library is organized into modular files that can be imported as needed using the `#import` directive.

## Import Syntax

```javascript
#import <stdlib/core.dmm>
#import <stdlib/math.dmm>
#import <stdlib/array.dmm>
#import <stdlib/strings.dmm>
#import <stdlib/utils.dmm>
#import <stdlib/io.dmm>
```

## Modules

### 1. I/O Module (`stdlib/io.dmm`)

**Input struct** for reading user input.

The Input struct provides methods for reading different types of user input:
- `init()` - Initialize the Input struct
- `readInt() -> int` - Read an integer from stdin (blocks until Enter is pressed)
- `readChar() -> char` - Read a character from stdin (blocks until Enter is pressed)
- `promptInt(prompt:string) -> int` - Print a prompt and read an integer
- `promptChar(prompt:string) -> char` - Print a prompt and read a character

**Note**: This module has compiler built-in support for scanf operations.

### 2. Core Module (`stdlib/core.dmm`)

**30 functions** providing fundamental operations for all data types.

#### Integer Operations
- `intAbs(n:int) -> int` - Returns absolute value
- `intMin(a:int, b:int) -> int` - Returns minimum
- `intMax(a:int, b:int) -> int` - Returns maximum
- `intClamp(value:int, min:int, max:int) -> int` - Clamps value to range
- `intIsEven(n:int) -> bit` - Checks if even
- `intIsOdd(n:int) -> bit` - Checks if odd
- `intIsPositive(n:int) -> bit` - Checks if positive
- `intIsNegative(n:int) -> bit` - Checks if negative
- `intIsZero(n:int) -> bit` - Checks if zero
- `intSign(n:int) -> int` - Returns -1, 0, or 1

#### String Operations
- `strEquals(a:string, b:string) -> bit` - Checks equality
- `strIsEmpty(s:string) -> bit` - Checks if empty
- `strIsNotEmpty(s:string) -> bit` - Checks if not empty

#### Boolean Operations
- `boolAnd(a:bit, b:bit) -> bit` - Logical AND
- `boolOr(a:bit, b:bit) -> bit` - Logical OR
- `boolNot(b:bit) -> bit` - Logical NOT
- `boolXor(a:bit, b:bit) -> bit` - Logical XOR
- `boolNand(a:bit, b:bit) -> bit` - Logical NAND
- `boolNor(a:bit, b:bit) -> bit` - Logical NOR

#### Byte Operations
- `byteAdd(a:byte, b:byte) -> byte` - Adds bytes
- `byteSub(a:byte, b:byte) -> byte` - Subtracts bytes
- `byteMin(a:byte, b:byte) -> byte` - Returns minimum
- `byteMax(a:byte, b:byte) -> byte` - Returns maximum

#### Character Operations
- `charIsUpper(c:char) -> bit` - Checks if uppercase
- `charIsLower(c:char) -> bit` - Checks if lowercase
- `charIsAlpha(c:char) -> bit` - Checks if letter
- `charIsDigit(c:char) -> bit` - Checks if digit
- `charIsAlnum(c:char) -> bit` - Checks if alphanumeric
- `charIsSpace(c:char) -> bit` - Checks if whitespace

### 2. Math Module (`stdlib/math.dmm`)

**28 functions** for mathematical operations, algorithms, and number theory.

#### Basic Math
- `mathPow(base:int, exp:int) -> int` - Power function
- `mathSquare(n:int) -> int` - Square of n
- `mathCube(n:int) -> int` - Cube of n
- `mathFactorial(n:int) -> int` - Factorial
- `mathFibonacci(n:int) -> int` - nth Fibonacci number

#### Summation
- `mathSum(n:int) -> int` - Sum 1 to n
- `mathSumRange(start:int, end:int) -> int` - Sum range
- `mathSumSquares(n:int) -> int` - Sum of squares 1 to n
- `mathSumCubes(n:int) -> int` - Sum of cubes 1 to n

#### Number Theory
- `mathGCD(a:int, b:int) -> int` - Greatest common divisor
- `mathLCM(a:int, b:int) -> int` - Least common multiple
- `mathIsPrime(n:int) -> bit` - Checks if prime
- `mathNthPrime(n:int) -> int` - Returns nth prime number
- `mathCountDivisors(n:int) -> int` - Counts divisors
- `mathSumDivisors(n:int) -> int` - Sums all divisors
- `mathIsPerfect(n:int) -> bit` - Checks if perfect number

#### Digit Operations
- `mathDigitCount(n:int) -> int` - Counts digits
- `mathDigitSum(n:int) -> int` - Sums digits
- `mathDigitReverse(n:int) -> int` - Reverses digits

#### Comparison Utilities
- `mathMin3(a:int, b:int, c:int) -> int` - Min of 3 values
- `mathMax3(a:int, b:int, c:int) -> int` - Max of 3 values
- `mathAverage(a:int, b:int) -> int` - Average of 2 values
- `mathAverage3(a:int, b:int, c:int) -> int` - Average of 3 values

### 3. Array Module (`stdlib/array.dmm`)

**25 functions** for array manipulation and analysis.

#### Array Initialization
- `arrayFillInt(arr[]:int, size:int, value:int) -> void`
- `arrayFillByte(arr[]:byte, size:int, value:byte) -> void`
- `arrayFillChar(arr[]:char, size:int, value:char) -> void`
- `arrayFillBit(arr[]:bit, size:int, value:bit) -> void`

#### Array Search
- `arrayIndexOfInt(arr[]:int, size:int, value:int) -> int`
- `arrayContainsInt(arr[]:int, size:int, value:int) -> bit`
- `arrayCountInt(arr[]:int, size:int, value:int) -> int`

#### Array Statistics
- `arrayMinInt(arr[]:int, size:int) -> int`
- `arrayMaxInt(arr[]:int, size:int) -> int`
- `arraySumInt(arr[]:int, size:int) -> int`
- `arrayAverageInt(arr[]:int, size:int) -> int`
- `arrayIndexOfMinInt(arr[]:int, size:int) -> int`
- `arrayIndexOfMaxInt(arr[]:int, size:int) -> int`

#### Array Manipulation
- `arrayReverseInt(arr[]:int, size:int) -> void`
- `arrayCopyInt(source[]:int, dest[]:int, size:int) -> void`
- `arrayMultiplyScalarInt(arr[]:int, size:int, scalar:int) -> void`
- `arrayAddScalarInt(arr[]:int, size:int, scalar:int) -> void`

#### Array Sorting
- `arraySortInt(arr[]:int, size:int) -> void`
- `arrayIsSortedInt(arr[]:int, size:int) -> bit`

#### Array Comparison
- `arrayEqualsInt(arr1[]:int, arr2[]:int, size:int) -> bit`

### 4. Strings Module (`stdlib/strings.dmm`)

**27 functions** for string manipulation and analysis.

#### String Analysis
- `strLength(s:string) -> int` - Returns string length
- `strCompare(s1:string, s2:string) -> int` - Compares strings
- `strStartsWith(s:string, c:char) -> bit` - Checks start character
- `strEndsWith(s:string, c:char) -> bit` - Checks end character
- `strCharAt(s:string, index:int) -> char` - Gets character at index
- `strIndexOf(s:string, c:char) -> int` - First occurrence of char
- `strLastIndexOf(s:string, c:char) -> int` - Last occurrence of char
- `strCountChar(s:string, c:char) -> int` - Counts character occurrences

#### String Classification
- `strIsAlpha(s:string) -> bit` - All letters
- `strIsDigit(s:string) -> bit` - All digits
- `strIsAlnum(s:string) -> bit` - All alphanumeric
- `strIsUpper(s:string) -> bit` - All uppercase
- `strIsLower(s:string) -> bit` - All lowercase

#### String Utilities
- `strPrintln(s:string) -> void` - Print with newline
- `strRepeatChar(c:char, n:int) -> void` - Print char n times
- `strCountWords(s:string) -> int` - Count words
- `strCountLines(s:string) -> int` - Count lines

#### String Transformation Helpers
- `strCharToDigit(c:char) -> int` - Convert char to digit
- `strDigitToChar(digit:int) -> char` - Convert digit to char
- `strParseInt(s:string) -> int` - Parse integer from string

### 5. Utils Module (`stdlib/utils.dmm`)

**35 functions** for general utility operations.

#### Comparison
- `min(a:int, b:int) -> int`
- `max(a:int, b:int) -> int`
- `min3(a:int, b:int, c:int) -> int`
- `max3(a:int, b:int, c:int) -> int`
- `min4(a:int, b:int, c:int, d:int) -> int`
- `max4(a:int, b:int, c:int, d:int) -> int`

#### Range and Boundary
- `clamp(value:int, min:int, max:int) -> int`
- `inRange(value:int, min:int, max:int) -> bit`
- `inRangeExclusive(value:int, min:int, max:int) -> bit`

#### Sign Operations
- `abs(n:int) -> int`
- `sign(n:int) -> int`
- `sameSign(a:int, b:int) -> bit`

#### Conditional Utilities
- `selectInt(condition:bit, a:int, b:int) -> int`
- `all3(a:bit, b:bit, c:bit) -> bit`
- `any3(a:bit, b:bit, c:bit) -> bit`
- `exactlyOne(a:bit, b:bit) -> bit`

#### Printing Utilities
- `printSeparator(c:char, length:int) -> void`
- `printLine() -> void`
- `printDoubleLine() -> void`
- `printNewlines(n:int) -> void`
- `printLabeled(label:string, value:int) -> void`
- `printBool(value:bit) -> void`
- `printLabeledBool(label:string, value:bit) -> void`

#### Validation
- `isValidIndex(index:int, size:int) -> bit`
- `isValidRange(start:int, end:int, size:int) -> bit`

#### Bit Manipulation
- `isBitSet(value:int, pos:int) -> bit`
- `countBits(value:int) -> int`

#### Safe Math
- `isSafeDiv(divisor:int) -> bit`
- `safeDivide(dividend:int, divisor:int) -> int`
- `safeModulo(dividend:int, divisor:int) -> int`

## Usage Examples

### Example 1: Using Core and Math Modules

```javascript
#import <stdlib/core.dmm>
#import <stdlib/math.dmm>

func main() -> void {
    var num:int = -42;
    print("Absolute value: " + intAbs(num));
    
    var result:int = mathPow(2, 10);
    print("2^10 = " + result);
    
    var fib:int = mathFibonacci(10);
    print("10th Fibonacci: " + fib);
}
```

### Example 2: Array Operations

```javascript
#import <stdlib/array.dmm>

func main() -> void {
    var[10] numbers:int;
    
    // Fill array with values
    for (var i:int = 0; i < 10; i++) {
        numbers[i] = i * 5;
    }
    
    // Find statistics
    var min:int = arrayMinInt(numbers, 10);
    var max:int = arrayMaxInt(numbers, 10);
    var sum:int = arraySumInt(numbers, 10);
    
    print("Min: " + min);
    print("Max: " + max);
    print("Sum: " + sum);
    
    // Sort array
    arraySortInt(numbers, 10);
}
```

### Example 3: String Analysis

```javascript
#import <stdlib/strings.dmm>

func main() -> void {
    var text:string = "Hello World";
    
    var len:int = strLength(text);
    print("Length: " + len);
    
    var index:int = strIndexOf(text, 'o');
    print("First 'o' at: " + index);
    
    var count:int = strCountChar(text, 'l');
    print("Count of 'l': " + count);
}
```

### Example 4: Using Utility Functions

```javascript
#import <stdlib/utils.dmm>

func main() -> void {
    printDoubleLine();
    print("My Program");
    printDoubleLine();
    
    var value:int = 75;
    var clamped:int = clamp(value, 0, 50);
    printLabeled("Clamped value", clamped);
    
    var inBounds:bit = inRange(25, 0, 100);
    printLabeledBool("In range", inBounds);
}
```

## Import Best Practices

1. **Import only what you need**: Import specific modules rather than all at once
2. **Avoid circular imports**: Don't create import cycles between files
3. **Use relative paths**: Place custom libraries in organized directories
4. **Document dependencies**: Comment which functions from imports you're using

## Future Enhancements

Planned additions to the standard library:
- File I/O operations
- Time and date functions
- Advanced string manipulation (split, join, replace)
- More data structures (linked lists, hash tables)
- Network operations
- Regular expressions

## Total Function Count

- **Core**: 30 functions
- **Math**: 28 functions
- **Array**: 25 functions
- **Strings**: 27 functions
- **Utils**: 35 functions

**Total**: 145 standard library functions

## Version

Standard Library Version: 1.0.0  
Compiler Version: 5.1.0+  
Date: November 2025
