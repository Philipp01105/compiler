# 📘 Compiler Documentation - Version 4.0.0 (Final Release)

**Author:** Philipp01105  
**Date:** 2025-10-16  
**Version:** 4.0.0 Final - Fully tested and production-ready  
**Status:** ✅ Production Ready - 7 Data Types + String Support + All Bugs Fixed

---

## 📑 Table of Contents

1. [Overview](#1-overview)
2. [Features & Capabilities](#2-features--capabilities)
3. [Installation & Setup](#3-installation--setup)
4. [Language Syntax & Reference](#4-language-syntax--reference)
5. [Practical Examples](#5-practical-examples)
6. [Compiler Architecture](#6-compiler-architecture)
7. [Test Suite & Quality Assurance](#7-test-suite--quality-assurance)
8. [Performance & Limits](#8-performance--limits)
9. [Known Limitations](#9-known-limitations)
10. [Changelog & Version History](#10-changelog--version-history)
11. [FAQ & Troubleshooting](#11-faq--troubleshooting)
12. [Developer Guide](#12-developer-guide)
13. [Roadmap & Future](#13-roadmap--future)

---

## 1. Overview

### 🎯 What is this?

This is a **fully functional compiler** for a custom C-like programming language that compiles to **x86-64 Assembly** (AT&T syntax). The compiler is written entirely in C (~4000 lines of code) and generates executable native code for **Windows (MinGW-w64)**.

### ⭐ Highlights

- ✅ **Professional 3-Phase Architecture**: Lexer → Parser → Code Generator
- ✅ **7 primitive data types**: int, char, byte, bit, float, double, **string**
- ✅ **Modern token-based lexer** with full UTF-8 support
- ✅ **Recursive descent parser** with operator precedence climbing
- ✅ **Native code generation** for x86-64 Windows
- ✅ **50 automated tests** - all passing ✅
- ✅ **String variables & literals** - fully functional
- ✅ **String functions** - parameters, return values, comparison
- ✅ **If/Else & Logical Operators** - fully functional
- ✅ **Backwards compatibility**: Old syntax (v2.x) still works
- ✅ **Type inference**: `var x = 5;` automatically becomes `int`
- ✅ **Production-ready**: Compiles complex programs like GCD, Fibonacci, Factorial
- ✅ **Robust error handling** with precise line and column information
- ✅ **Debugging support** with `--debug` and `--tokens` flags

### 🔥 Core Features

| Feature | Status | Description |
|---------|--------|-------------|
| **Functions** | ✅ | Up to 10 parameters, return values, nested calls, 7 types |
| **Variables** | ✅ | 7 data types, scope management (Function/Loop/If), up to 200 simultaneously |
| **Data Types** | ✅ | int, char, byte, bit, float, double, **string** |
| **Operators** | ✅ | Arithmetic (+,-,*,/,%), Comparison (<,<=,>,>=,==,!=), Logic (&&,\|\|,!), Assignment (+=,-=,*=,/=) |
| **Control Flow** | ✅ | for-loops and if/else with arbitrary nesting |
| **Expressions** | ✅ | Parentheses, operator precedence, nested function calls, unary operators |
| **I/O** | ✅ | print() with string concatenation, all types |
| **Strings** | ✅ | String variables, literals, parameters, return values, comparison |
| **UTF-8** | ✅ | Fully supported in strings |
| **Debug Mode** | ✅ | Detailed compiler output with assembly comments |
| **Scope Management** | ✅ | Correct variable cleanup in nested scopes |

---

## 2. Features & Capabilities

### 2.1 Data Types (NEW in 4.0!)

The compiler supports **7 primitive data types**:

| Type | Size | Range | Description |
|------|------|-------|-------------|
| **int** | 4 bytes | -2,147,483,648 to 2,147,483,647 | 32-bit signed integer |
| **char** | 1 byte | -128 to 127 | 8-bit signed character (ASCII) |
| **byte** | 1 byte | 0 to 255 | 8-bit unsigned integer |
| **bit** | 1 byte | 0 or 1 | Boolean (0 = false, 1 = true) |
| **float** | 4 bytes | ±3.4E±38 | 32-bit floating point (IEEE 754) |
| **double** | 8 bytes | ±1.7E±308 | 64-bit floating point (IEEE 754) |
| **string** | 8 bytes | Pointer | 64-bit pointer to string literal |

#### Examples

```javascript
// Integer
var age:int = 25;
var negative:int = -42;

// Character
var initial:char = 'A';
var newline:char = '\n';
var tab:char = '\t';

// Byte (unsigned)
var pixelValue:byte = 255;
var counter:byte = 0;

// Bit (Boolean)
var isValid:bit = 1;
var hasError:bit = 0;

// Float
var pi:float = 3.14159;
var temperature:float = -40.5;

// Double
var e:double = 2.718281828459045;
var distance:double = 1.5e10;

// String (NEW!)
var name:string = "Alice";
var greeting:string = "Hello World";
var empty:string = "";
```

### 2.2 String Support (NEW in 4.0!)

#### String Variables

```javascript
// Declaration with initialization
var name:string = "Philipp";
var city:string = "Berlin";
var empty:string = "";

// Print string variables
print(name);          // Output: Philipp
print("City: " + city);  // Output: City: Berlin
```

#### String Assignment

```javascript
// String literal assignment
var msg:string;
msg = "Hello";
print(msg);  // Output: Hello

// Copy string variable (pointer copy)
var original:string = "Original";
var copy:string = original;
print(copy);  // Output: Original
```

#### String Comparison

```javascript
var str1:string = "Test";
var str2:string = "Test";
var str3:string = "Different";

// Equality check
if (str1 == str2) {
    print("Equal!");  // This will print
}

// Inequality check
if (str1 != str3) {
    print("Not equal!");  // This will print
}
```

#### String Functions

```javascript
// String parameter
func greet(name:string) -> void {
    print("Hello, " + name + "!");
}

// String return value
func getWelcomeMessage() -> string {
    return "Welcome";
}

// Multiple string parameters
func combineNames(first:string, last:string) -> void {
    print(first + " " + last);
}

func main() -> void {
    greet("Alice");  // Output: Hello, Alice!
    
    var msg:string = getWelcomeMessage();
    print(msg);  // Output: Welcome
    
    combineNames("Max", "Mustermann");  // Output: Max Mustermann
}
```

#### String in Control Flow

```javascript
func main() -> void {
    var role:string = "admin";
    
    // String in if-statement
    if (role == "admin") {
        print("Welcome, Administrator!");
        print("Full access granted");
    } else {
        print("Welcome, User!");
    }
    
    // String in for-loop
    for (var i:int = 0; i < 3; i++) {
        var status:string = "Iteration";
        print(status + " " + i);
    }
}
```

### 2.3 Functions

The compiler supports full function definitions with **typed parameters** and return values.

#### Syntax

```javascript
func <name>(<param>:<type>, ...) -> <return_type>
{
    <body>
}
```

#### Examples

```javascript
// Function with typed parameters
func add(a:int, b:int) -> int
{
    return a + b;
}

// String function
func makeGreeting(name:string) -> string {
    return name;  // Returns the string pointer
}

// Multiple string parameters
func printThreeStrings(a:string, b:string, c:string) -> void {
    print(a);
    print(b);
    print(c);
}

// Void function
func printSeparator() -> void
{
    print("=================================");
}
```

#### Function Features

| Feature | Limit | Description |
|---------|-------|-------------|
| **Parameters** | Max. 10 | All 7 data types supported |
| **Return value** | 7 types + void | int, char, byte, bit, float, double, string, void |
| **Functions per program** | Max. 100 | Forward declarations possible |
| **Nesting depth** | Unlimited | Functions can be called arbitrarily deep |
| **Recursion** | ⚠️ Not recommended | Stack limit at ~100 levels |

**Mandatory:** Every program **must** have a `main()` function:

```javascript
func main() -> void
{
    print("Hello World!");
}
```

### 2.4 Variables

Variables can be declared with **three different syntaxes**:

#### Syntax Variants

```javascript
// 1. Old syntax (Type Inference) - Backwards compatible
var x = 10;              // ✅ Automatically 'int'

// 2. New syntax with type and initialization
var x:int = 10;          // ✅ Explicit type

// 3. New syntax without initialization
var x:int;               // ✅ Initialized with 0

// 4. String variables (NEW!)
var name:string = "Alice";
var empty:string;  // Initialized with empty string pointer
```

#### Examples

```javascript
// Type Inference (old syntax)
var age = 25;                    // int
var sum = 10 + 20;              // int

// Explicit types (new syntax)
var initial:char = 'P';
var pi:float = 3.14159;
var isValid:bit = 1;
var maxByte:byte = 255;
var name:string = "Philipp";  // String!

// Uninitialized variables
var counter:int;                // Initialized with 0
var result:float;               // Initialized with 0.0
var flag:bit;                   // Initialized with 0
var msg:string;                 // Initialized with null pointer

counter = 5;
result = 3.14;
flag = 1;
msg = "Hello";

// With calculations
var product:int = age * 2;
var area:float = pi * 5.0 * 5.0;

// With function calls
var factorial5:int = factorial(5);
var greeting:string = getWelcomeMessage();  // String return!

// With logical expressions
var bothPositive:bit = x > 0 && y > 0;
```

#### Variable Limits

| Property | Value |
|----------|-------|
| **Max. variables simultaneously** | 200 |
| **Supported types** | 7 (int, char, byte, bit, float, double, string) |
| **Initialization** | Optional (with type annotation) |
| **Scope levels** | 3+ (Function, Loop, If/Else, nested) |
| **Name length** | Max. 256 characters |

### 2.5 Operators

#### 2.5.1 Arithmetic Operators

```javascript
var a:int = 10;
var b:int = 3;

var sum:int = a + b;      // 13
var diff:int = a - b;     // 7
var prod:int = a * b;     // 30
var quot:int = a / b;     // 3 (Integer division!)
var mod:int = a % b;      // 1 (Modulo - NEW!)
var neg:int = -a;         // -10 (Unary minus)
```

#### 2.5.2 Comparison Operators

```javascript
var a:int = 10;
var b:int = 20;

var isLess:bit = a < b;       // 1 (true)
var isGreater:bit = a > b;    // 0 (false)
var isEqual:bit = a == b;     // 0 (false)
var isNotEqual:bit = a != b;  // 1 (true)
var isLessEq:bit = a <= b;    // 1 (true)
var isGreaterEq:bit = a >= b; // 0 (false)

// String comparison (NEW!)
var str1:string = "Hello";
var str2:string = "Hello";
var same:bit = str1 == str2;  // 1 (pointer comparison)
```

#### 2.5.3 Logical Operators

```javascript
var a:int = 5;
var b:int = 10;

// AND - Both conditions must be true
var both:bit = a > 0 && b > 0;  // 1 (true)

// OR - At least one condition must be true
var either:bit = a == 0 || b == 10;  // 1 (true)

// NOT - Inverts the truth value
var notEqual:bit = !(a == b);  // 1 (true)

// Complex expressions with parentheses
var complex:bit = (a > 0 && b > 0) || a == 100;  // 1 (true)
```

#### 2.5.4 Modulo Operator (NEW in 4.0!)

```javascript
func main() -> void {
    var a:int = 10;
    var b:int = 3;
    
    var remainder:int = a % b;  // 1
    print("10 mod 3 = " + remainder);
    
    // Check if even
    var isEven:bit = (a % 2) == 0;  // 1 (true for 10)
}

// Modulo in functions
func mod(a:int, b:int) -> int {
    return a % b;
}
```

### 2.6 Print Statement

The `print()` statement supports string concatenation with **all data types**, including strings.

#### Examples

```javascript
// Integer
var x:int = 42;
print("x = " + x);

// Character
var ch:char = 'A';
print("Char: " + ch);

// Float
var pi:float = 3.14;
print("Pi: " + pi);

// Bit
var flag:bit = 1;
print("Flag: " + flag);

// String (NEW!)
var name:string = "Alice";
print("Name: " + name);
print("Hello, " + name + "!");

// Mixed
print("Int: " + x + ", Char: " + ch + ", Name: " + name);

// String concatenation
var greeting:string = "Hello";
var target:string = "World";
print(greeting + ", " + target + "!");  // Output: Hello, World!
```

---

## 3. Installation & Setup

### 3.1 Prerequisites

#### Windows

- **GCC (MinGW-w64)** - [Download](https://www.mingw-w64.org/) or [w64devkit](https://github.com/skeeto/w64devkit)
- **CMake** (3.10+) - [Download](https://cmake.org/download/)
- Optional: **CLion**, **VS Code** with C/C++ Extension

#### Linux/Mac

- **GCC** - `sudo apt install gcc` (Ubuntu) or `brew install gcc` (Mac)
- **CMake** - `sudo apt install cmake` or `brew install cmake`

**Note:** The compiler generates Windows x64 assembly. For Linux/Mac, the code generator needs to be adapted (SystemV ABI).

### 3.2 Compiling the Compiler

#### With CMake (recommended)

```bash
# Clone repository
git clone https://github.com/Philipp01105/compiler.git
cd compiler

# Create build directory
mkdir build
cd build

# Configure CMake
cmake ..

# Compile
cmake --build .

# Or with make
make
```

### 3.3 Verify Installation

```bash
# Show compiler version
./compiler --help

# Compile test program
cat > ../test.txt << 'EOF'
func main() -> void {
    var name:string = "Philipp";
    var age:int = 42;
    var grade:char = 'A';
    var pi:float = 3.14;
    
    print("Name: " + name);
    print("Age: " + age);
    print("Grade: " + grade);
    print("Pi: " + pi);
    
    if (name == "Philipp") {
        print("Hello, Philipp!");
    }
}
EOF

./compiler ../test.txt
gcc -no-pie ../test.txt.s -o test
./test
```

**Expected output:**
```
Name: Philipp
Age: 42
Grade: A
Pi: 3.140000
Hello, Philipp!
```

---

## 7. Test Suite & Quality Assurance

### 7.1 Test Coverage

The compiler is tested with **50 automated tests** that cover **all features**:

| Test Category | Tests | Status |
|---------------|-------|--------|
| **Variables & Types** | 7 | ✅ All passed |
| **Arithmetic Operators** | 5 | ✅ All passed |
| **Comparison Operators** | 2 | ✅ All passed |
| **Logical Operators** | 2 | ✅ All passed |
| **If/Else** | 3 | ✅ All passed |
| **For-Loops** | 4 | ✅ All passed |
| **Functions** | 7 | ✅ All passed |
| **Nested Calls** | 3 | ✅ All passed |
| **Algorithms** | 6 | ✅ All passed |
| **Strings (NEW!)** | 11 | ✅ All passed |

### 7.2 String Test Examples

**TEST 32: String Variable Declaration**
```
Name: Philipp         ✅
City: Berlin          ✅
Country: Germany      ✅
Empty string: ''      ✅
```

**TEST 33: String Assignment**
```
Before assignment (should be empty)
After 1st assignment: Hello     ✅
After 2nd assignment: Guten Tag ✅
After 3rd assignment: Bonjour   ✅
```

**TEST 35: String Comparison (==)**
```
str1: Test
str2: Test
str3: Different
str1 == str2: TRUE (correct!)   ✅
str1 == str3: FALSE (correct!)  ✅
```

**TEST 39: String Functions (Return)**
```
getWelcomeMessage(): Welcome    ✅
getDefaultName(): Guest         ✅
getEmptyString(): ''            ✅
```

**TEST 41: String Functions (with return)**
```
Greeting for Alice: Alice       ✅
Greeting for Bob: Bob           ✅
```

---

## 10. Changelog & Version History

### Version 4.0.0 (2025-10-16) - **STRING SUPPORT RELEASE** 🎉

**Status:** ✅ Production Ready - 50/50 Tests Passed

**Major New Features:**
- ✅ **String data type**: Full support for string variables
- ✅ **String literals**: `"Hello World"`
- ✅ **String variables**: `var name:string = "Alice";`
- ✅ **String parameters**: `func greet(name:string) -> void`
- ✅ **String return values**: `func getName() -> string`
- ✅ **String assignment**: `name = "Bob";`
- ✅ **String comparison**: `if (name == "admin")`
- ✅ **String in print()**: `print("Hello, " + name + "!");`
- ✅ **Modulo operator**: `%` for remainder calculation

**Technical Implementation:**
- String type: 8-byte pointer (64-bit)
- String literals: Stored in `.rdata` section with proper escaping
- Parameter passing: Uses 64-bit registers (RCX, RDX, R8, R9)
- Return values: String pointers in RAX (64-bit)
- Comparison: Pointer-based (works for string literals)
- Print format: `printf("%s")` with proper register alignment

**Critical Bugfixes:**
- ✅ **Parameter offsets**: Fixed size calculation for 8-byte types (strings, doubles)
- ✅ **Stack alignment**: 8-byte types properly aligned to 8-byte boundaries
- ✅ **Return values**: String pointers preserved in full 64-bit RAX register
- ✅ **Print statement**: Correct register usage (%rdx for string pointer, %rcx for format)
- ✅ **Stack size**: Increased to 8192 bytes for programs with many variables

**Test Results:**
- ✅ 50/50 tests passed
- ✅ 11 new string tests
- ✅ All previous tests still passing

**Breaking Changes:**
- ❌ **None!** All previous programs still work

**Lines of Code:** ~4000 (previously: ~3500)

### Version 3.0.4 (2025-10-15) - **BUGFIX RELEASE**

**Status:** ✅ Production Ready - All critical bugs fixed

**Critical Bugfixes:**
- ✅ **Offset calculation**: Correct stack offset for all variable types
- ✅ **Scope cleanup**: Variables marked as deleted (scope == -1), not removed
- ✅ **Variable lookup**: Skip deleted variables correctly
- ✅ **Byte/Bit print**: Correct `movzbl` instruction when loading
- ✅ **Logical operators**: Clean boolean values (0/1) guaranteed
- ✅ **Return statement**: Zero-extend with `movl %eax, %eax`
- ✅ **Uninitialized variables**: Correct initialization with 0

### Version 3.0.1 (2025-10-15) - **DATA TYPES RELEASE**

**Status:** ✅ Production Ready - 6 data types + backwards compatibility

**New Features:**
- ✅ **6 primitive data types**: int, char, byte, bit, float, double
- ✅ **Character literals**: `'A'`, `'\n'`, `'\t'` with escape sequences
- ✅ **Float literals**: `3.14`, `1.5e10`, scientific notation
- ✅ **Type inference**: `var x = 5;` automatically becomes `int`
- ✅ **Explicit types**: `var x:int = 5;`
- ✅ **Uninitialized variables**: `var x:int;` (initialized with 0)
- ✅ **Backwards compatibility**: Old syntax (v2.x) still works

---

## 11. FAQ & Troubleshooting

### 11.1 Frequently Asked Questions

#### Q: How do strings work internally?

**A:** Strings are 64-bit pointers to string literals stored in the `.rdata` section:

```javascript
var name:string = "Alice";  // name contains pointer to "Alice"
```

- String literals are deduplicated (same text = same pointer)
- String comparison compares pointers (works for literals)
- String assignment copies the pointer (shallow copy)

#### Q: Can I modify strings?

**A:** Not yet! Strings are currently immutable (read-only). For Version 4.5 planned:
```javascript
// Planned for 4.5:
var name:string = "Alice";
name[0] = 'B';  // ❌ Not yet available
```

#### Q: Can I concatenate strings at runtime?

**A:** Only in `print()` statements:
```javascript
// ✅ Works in print
print("Hello, " + name + "!");

// ❌ Not yet supported
var greeting:string = "Hello, " + name;
```

#### Q: How do I compare strings for content?

**A:** Currently, string comparison uses pointer equality. For string literals, this works:
```javascript
var role:string = "admin";
if (role == "admin") {  // ✅ Works (same literal)
    print("Access granted");
}
```

For Version 4.5 planned: `strcmp()` function for content comparison.

#### Q: Can I pass strings to functions?

**A:** Yes! Fully supported:
```javascript
func greet(name:string) -> void {
    print("Hello, " + name + "!");
}

func getName() -> string {
    return "Alice";
}

func main() -> void {
    greet("Bob");                    // ✅ String literal as argument
    var msg:string = getName();      // ✅ String return value
    print(msg);                      // ✅ String variable in print
}
```

---

## 13. Roadmap & Future

### Version 4.5 (Q1 2026) - String Operations

**Planned Features:**
- ✅ **String concatenation**: `var full = first + " " + last;`
- ✅ **String comparison**: `strcmp()` for content comparison
- ✅ **String length**: `strlen()` function
- ✅ **String indexing**: `name[0]` to access characters
- ✅ **String mutation**: Modify string contents

### Version 5.0 (Q2 2026) - Advanced Types

**Planned Features:**
- ✅ **Arrays**: `var arr:int[10];`
- ✅ **Structs**: Custom data types
- ✅ **Pointers**: `var ptr:int*;`
- ✅ **Dynamic strings**: Heap-allocated, mutable strings

### Version 6.0 (Q3 2026) - Advanced Features

**Planned Features:**
- ✅ **Classes**: OOP support
- ✅ **Heap allocation**: malloc/free
- ✅ **Tail-call optimization**: Efficient recursion
- ✅ **Generics**: Template system

---

# 🎉 COMPILER VERSION 4.0.0 - DOCUMENTATION END 🎉

**Author:** Philipp01105  
**Date:** 2025-10-16 07:43:32 UTC  
**Status:** ✅ Production Ready  
**Features:** 7 Data Types (int, char, byte, bit, float, double, **string**) + If/Else + Logical Operators + Modulo + All Bugs Fixed

**Repository:** https://github.com/Philipp01105/compiler

---

**Summary of changes in 4.0.0:**
- ✅ Version updated to 4.0.0
- ✅ String support fully documented
- ✅ 50 test cases (11 new string tests)
- ✅ Modulo operator documented
- ✅ String examples throughout documentation
- ✅ Updated date (2025-10-16 07:43:32 UTC)
- ✅ Lines of code updated (~4000 instead of ~3500)
- ✅ Status badges updated
- ✅ Test status updated to 50/50
- ✅ Technical implementation details for strings
- ✅ FAQ expanded with string-specific questions
- ✅ Roadmap updated with string operations

*This documentation fully describes all features of version 4.0.0 including full string support*