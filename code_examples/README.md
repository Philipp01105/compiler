# DMM Language Code Examples

This directory contains comprehensive examples demonstrating all features of the DMM programming language. Each example is self-contained and focuses on specific language features.

## 📚 Example Index

### Basic Examples

1. **[01_hello_world.dmm](01_hello_world.dmm)** - Hello World
   - Basic program structure
   - `println()` function
   - String literals

2. **[02_variables_and_types.dmm](02_variables_and_types.dmm)** - Variables and Data Types
   - All 7 data types: `int`, `char`, `byte`, `bit`, `float`, `double`, `string`
   - Variable declaration and initialization
   - Type inference
   - Printing different types

3. **[03_operators.dmm](03_operators.dmm)** - Operators
   - Arithmetic operators: `+`, `-`, `*`, `/`, `%`
   - Comparison operators: `<`, `<=`, `>`, `>=`, `==`, `!=`
   - Logical operators: `&&`, `||`, `!`
   - Operator precedence
   - Complex expressions

4. **[04_control_flow.dmm](04_control_flow.dmm)** - Control Flow
   - `if` / `else` statements
   - `else if` chains
   - Nested conditionals
   - `for` loops
   - Nested loops
   - Loop conditions

5. **[05_functions.dmm](05_functions.dmm)** - Functions
   - Function parameters
   - Return values (all types)
   - Multiple parameters
   - Recursion (factorial, fibonacci)
   - Function calls
   - Nested function calls

6. **[06_strings.dmm](06_strings.dmm)** - String Operations
   - String variables
   - String concatenation
   - String comparison (`==`, `!=`)
   - Strings with numbers
   - String parameters and return values
   - Strings in control flow

### Intermediate Examples

7. **[07_arrays.dmm](07_arrays.dmm)** - Arrays
   - Array declaration with size
   - Array element access and assignment
   - Iterating through arrays
   - Array calculations
   - Finding max/min in arrays
   - Arrays in functions

8. **[08_structs_basic.dmm](08_structs_basic.dmm)** - Basic Structs
   - Struct definition with fields
   - Field access and assignment
   - Multiple struct instances
   - Structs with different types
   - Modifying struct fields

9. **[09_structs_methods.dmm](09_structs_methods.dmm)** - Structs with Methods
   - Struct methods accessing fields
   - Methods with parameters
   - Methods with return values
   - Methods modifying state
   - Object-oriented style programming

10. **[10_structs_nested.dmm](10_structs_nested.dmm)** - Nested Structs
    - Structs containing other structs
    - Accessing nested fields
    - Calculations with nested structs
    - Moving and transforming nested structures

### Advanced Examples

11. **[11_algorithms.dmm](11_algorithms.dmm)** - Common Algorithms
    - Factorial (recursion)
    - Fibonacci (recursion)
    - GCD (Euclidean algorithm)
    - Prime number checking
    - Power function
    - Mathematical utilities (abs, max, min)
    - Sum of digits

12. **[12_simple_calculator.dmm](12_simple_calculator.dmm)** - Calculator Application
    - Struct-based state management
    - Multiple mathematical operations
    - Memory functions
    - Chain calculations
    - Real-world application structure

### Game Examples

13. **[tictactoe.dmm](tictactoe.dmm)** - Tic-Tac-Toe Game *(existing)*
14. **[guess_number.dmm](guess_number.dmm)** - Number Guessing Game *(existing)*
15. **[rock_paper_scissors.dmm](rock_paper_scissors.dmm)** - Rock Paper Scissors *(existing)*
16. **[calculator.dmm](calculator.dmm)** - Advanced Calculator *(existing)*
17. **[shapes.dmm](shapes.dmm)** - Geometric Shapes *(existing)*

## 🚀 How to Compile and Run

### Step 1: Build the Compiler

```bash
cd /path/to/compiler
mkdir build && cd build
cmake ..
make
```

### Step 2: Compile an Example

```bash
./build/compiler code_examples/01_hello_world.dmm
```

This generates `code_examples/01_hello_world.dmm.s` (assembly file).

### Step 3: Assemble and Link

```bash
gcc -no-pie code_examples/01_hello_world.dmm.s -o hello
```

### Step 4: Run the Program

```bash
./hello
```

### Complete Example

```bash
# Compile all examples
for example in code_examples/*.dmm; do
    ./build/compiler "$example"
    gcc -no-pie "${example}.s" -o "${example%.dmm}"
done

# Run an example
./code_examples/01_hello_world
```

## 📖 Language Features Covered

### Data Types (7 types)

```javascript
var age:int = 25;              // 32-bit signed integer
var initial:char = 'A';        // 8-bit character
var pixel:byte = 255;          // 8-bit unsigned
var flag:bit = 1;              // Boolean (0 or 1)
var pi:float = 3.14;           // 32-bit float
var e:double = 2.718;          // 64-bit double
var name:string = "Alice";     // String pointer
```

### Control Structures

```javascript
// if/else
if (x > 10) {
    println("Greater");
} else {
    println("Not greater");
}

// else if chains
if (score >= 90) {
    println("A");
} else if (score >= 80) {
    println("B");
} else {
    println("C");
}

// for loops
for (var i:int = 0; i < 10; i++) {
    println(i);
}
```

### Functions

```javascript
// Function with parameters and return value
func add(a:int, b:int) -> int {
    return a + b;
}

// Recursive function
func factorial(n:int) -> int {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}
```

### Arrays

```javascript
// Array declaration
var[10] numbers:int;

// Array access
numbers[0] = 42;
println(numbers[0]);

// Array iteration
for (var i:int = 0; i < 10; i++) {
    numbers[i] = i * 2;
}
```

### Structs

```javascript
// Struct definition
struct Point {
    var x:int;
    var y:int;
    
    func init(xVal:int, yVal:int) -> void {
        x = xVal;
        y = yVal;
    }
    
    func print() -> void {
        println("Point(" + x + ", " + y + ")");
    }
}

// Struct usage
func main() -> void {
    var p:Point;
    p.init(10, 20);
    p.print();
}
```

### Nested Structs

```javascript
struct Point {
    var x:int;
    var y:int;
}

struct Rectangle {
    var topLeft:Point;
    var bottomRight:Point;
}

func main() -> void {
    var rect:Rectangle;
    rect.topLeft.x = 0;
    rect.topLeft.y = 0;
    rect.bottomRight.x = 10;
    rect.bottomRight.y = 5;
}
```

## 🎯 Learning Path

### Beginners
Start with examples 01-06 to learn basic syntax, data types, and control flow.

### Intermediate
Continue with examples 07-10 to understand arrays and structs.

### Advanced
Explore examples 11-12 and the game examples for complex patterns and algorithms.

## 📝 Example Template

When creating new examples, follow this template:

```javascript
// =================================================================
// EXAMPLE TITLE
// Brief description
// =================================================================
// This example demonstrates:
// - Feature 1
// - Feature 2
// - Feature 3
// =================================================================

func main() -> void {
    println("=== EXAMPLE DEMONSTRATION ===");
    println("");
    
    // Your code here
    
    println("=== DEMONSTRATION COMPLETE ===");
}
```

## 🔍 Built-in Functions

The language provides two built-in I/O functions:

- **`print(expr)`** - Print without newline
- **`println(expr)`** - Print with newline

Both support string concatenation with all data types:

```javascript
var x:int = 42;
var name:string = "Alice";
println("Name: " + name + ", Value: " + x);
```

## ⚙️ Compilation Details

### Assembly Output

The compiler generates x86-64 AT&T syntax assembly:
- Linux syscalls for I/O
- Stack-based variable storage
- Register-based expression evaluation

### Compiler Flags

```bash
./build/compiler [options] <input.dmm>

Options:
  --debug      Print detailed compilation information
  --tokens     Show token stream
```

## 🧪 Testing Examples

To verify all examples compile correctly:

```bash
#!/bin/bash
for example in code_examples/*.dmm; do
    echo "Testing $example..."
    ./build/compiler "$example" > /dev/null 2>&1
    if [ $? -eq 0 ]; then
        echo "  ✓ OK"
    else
        echo "  ✗ FAILED"
    fi
done
```

## 💡 Tips and Best Practices

1. **Variable Initialization**: Always initialize variables before use
2. **Type Safety**: Use explicit types (`:int`, `:string`, etc.) for clarity
3. **Function Size**: Keep functions focused on a single task
4. **Struct Methods**: Use methods to encapsulate behavior with data
5. **Comments**: Add clear comments to explain complex logic
6. **Testing**: Test edge cases (division by zero, array bounds, etc.)

## 🐛 Common Pitfalls

1. **Division by Zero**: Always check divisor before division
2. **Array Bounds**: Ensure array indices are within declared size
3. **Uninitialized Variables**: Variables default to 0 but should be explicitly set
4. **String Comparison**: Use `==` for comparison, not assignment `=`
5. **Recursion Depth**: Deep recursion can cause stack overflow

## 📊 Performance Notes

- **Recursion**: Can be slow for large inputs (e.g., fibonacci)
- **String Concat**: Efficient for simple concatenation
- **Loops**: Prefer iteration over recursion for better performance
- **Struct Methods**: No overhead compared to regular functions

## 🔗 Additional Resources

- **Language Documentation**: See `examples/LANGUAGE_DOCUMENTATION.md`
- **Compiler Architecture**: See main `README.md`
- **Test Suite**: See `tests/` directory for more examples
- **Issue Tracker**: GitHub issues for bug reports and feature requests

## 📜 License

These examples are part of the DMM compiler project.

## ✨ Contributing

To add new examples:
1. Follow the template structure
2. Add clear comments explaining features
3. Test compilation and execution
4. Update this README with the new example
5. Ensure no dependencies on external libraries

---

**Version**: 1.0.0  
**Last Updated**: December 2025  
**Compiler Version**: 5.1.0+

All examples are self-contained and use only built-in language features.
