# DMM Compiler

A compiler for the DMM programming language that compiles to x86-64 assembly.

## Features

- **Data Types**: int, char, byte, bit, string, arrays, structs, pointers
- **Control Flow**: if/else, else if chains, for loops
- **Functions**: User-defined functions with parameters and return values
- **Operators**: Arithmetic (+, -, *, /, %), comparison, logical operators
- **Built-in I/O**: print() and println() functions with string concatenation
- **Structs**: Object-oriented features with methods and fields
- **Arrays**: Static arrays with compile-time size specification
- **Heap Management**: Pointers, reserve, free, and garbage collection

## Building the Compiler

### Prerequisites

- CMake 3.10 or higher
- GCC or compatible C compiler
- Make

### Build Steps

```bash
mkdir build
cd build
cmake ..
make
```

The compiler executable will be created at `build/compiler`.

## Using the Compiler

### Compile a DMM Program

```bash
./build/compiler source.dmm
```

This generates `source.dmm.s` (assembly file).

### Assemble and Link

```bash
gcc -no-pie source.dmm.s -o program
```

### Run the Program

```bash
./program
```

### Complete Example

```bash
# Compile
./build/compiler examples/hello.dmm

# Assemble
gcc -no-pie examples/hello.dmm.s -o hello

# Run
./hello
```

## Testing

The project includes an automatic testing system that compiles, runs, and verifies test programs.

### Run All Tests

```bash
./run_tests.sh
```

This will:
1. Find all `.dmm` files in the `tests/` directory
2. Compile each test to assembly
3. Assemble to executable
4. Run the program and capture output
5. Compare output against expected results
6. Report pass/fail status

### Test Results

```
=================================================================
           DMM COMPILER - AUTOMATIC TEST RUNNER
=================================================================

Test 1: hello
-------------------------------------------------------------------
  Compiling hello.dmm... OK
  Running test... OK
  Comparing output... MATCH
✓ TEST PASSED

=================================================================
                     TEST SUMMARY
=================================================================

Total Tests:  9
Passed:       9
Failed:       0

✓ ALL TESTS PASSED
```

### Adding New Tests

1. Create a test file in `tests/`:
   ```bash
   nano tests/my_test.dmm
   ```

2. Run the test runner to generate output:
   ```bash
   ./run_tests.sh
   ```

3. Copy the output as the expected result:
   ```bash
   cp test_output/my_test.out tests/expected/my_test.expected
   ```

4. Run tests again to verify:
   ```bash
   ./run_tests.sh
   ```

See [tests/README.md](../tests/README.md) for detailed testing documentation.

## Language Examples

For comprehensive examples showcasing all language features, see the `code_examples/` directory and its [README](../code_examples/README.md).

### Hello World

```javascript
func main() -> void {
    println("Hello, World!");
}
```

### Functions and Variables

```javascript
func add(x:int, y:int) -> int {
    return x + y;
}

func main() -> void {
    var result:int = add(5, 3);
    println("5 + 3 = " + result);
}
```

### Loops and Conditionals

```javascript
func main() -> void {
    for (var i:int = 1; i <= 5; i++) {
        if (i % 2 == 0) {
            println(i + " is even");
        } else {
            println(i + " is odd");
        }
    }
}
```

### Structs with Methods

```javascript
struct Point {
    var x:int;
    var y:int;
    
    func move(dx:int, dy:int) -> void {
        x = x + dx;
        y = y + dy;
    }
    
    func print() -> void {
        println("Point(" + x + ", " + y + ")");
    }
}

func main() -> void {
    var p:Point;
    p.x = 10;
    p.y = 20;
    p.print();
    p.move(5, 3);
    p.print();
}
```

### Arrays

```javascript
func main() -> void {
    var[5] numbers:int;
    
    // Fill array
    for (var i:int = 0; i < 5; i++) {
        numbers[i] = i * 10;
    }
    
    // Print array
    for (var i:int = 0; i < 5; i++) {
        println("numbers[" + i + "] = " + numbers[i]);
    }
}
```

## Project Structure

```
.
├── build/              # Build artifacts (generated)
├── code_examples/      # Example programs demonstrating all features
├── examples/           # Language documentation
├── src/                # Compiler source code
│   ├── lexer.c         # Lexical analysis
│   ├── parser_*.c      # Syntax analysis and code generation
│   └── main.c          # Compiler entry point
├── tests/              # Test suite
│   ├── expected/       # Expected test outputs
│   ├── *.dmm           # Test programs
│   └── README.md       # Testing documentation
├── CMakeLists.txt      # Build configuration
├── run_tests.sh        # Automatic test runner
└── README.md           # This file
```

## Built-in Functions

The language provides two built-in I/O functions:

- **`print(expr)`** - Print expression without newline
- **`println(expr)`** - Print expression with newline

Both functions support string concatenation with all data types using the `+` operator:

```javascript
func main() -> void {
    var name:string = "Alice";
    var age:int = 25;
    println("Name: " + name + ", Age: " + age);
}
```

## Example Programs

The `code_examples/` directory contains comprehensive working examples:

### Basic Examples (01-06)
- **01_hello_world.dmm**: Basic program structure
- **02_variables_and_types.dmm**: All 7 data types
- **03_operators.dmm**: Arithmetic, comparison, logical
- **04_control_flow.dmm**: if/else, loops
- **05_functions.dmm**: Parameters, return values, recursion
- **06_strings.dmm**: String operations and concatenation

### Intermediate Examples (07-10)
- **07_arrays.dmm**: Array declaration and manipulation
- **08_structs_basic.dmm**: Struct fields and instances
- **09_structs_methods.dmm**: Methods and encapsulation
- **10_structs_nested.dmm**: Nested struct composition

### Advanced Examples (11-12)
- **11_algorithms.dmm**: Factorial, Fibonacci, GCD, primes
- **12_simple_calculator.dmm**: Calculator with memory

### Game Examples
- **calculator.dmm**: Advanced calculator with operations
- **tictactoe.dmm**: Tic-tac-toe with AI
- **guess_number.dmm**: Number guessing game
- **rock_paper_scissors.dmm**: Rock-paper-scissors tournament
- **shapes.dmm**: Geometric shapes with nested structs
- **heap_management_demo.dmm**: Pointer and heap management

See [code_examples/README.md](../code_examples/README.md) for comprehensive details on all examples.

## Testing Your Changes

After making changes to the compiler:

1. **Rebuild the compiler**:
   ```bash
   cd build && make && cd ..
   ```

2. **Run the test suite**:
   ```bash
   ./run_tests.sh
   ```

3. **Test with example programs**:
   ```bash
   ./build/compiler code_examples/calculator.dmm
   gcc -no-pie code_examples/calculator.dmm.s -o calculator
   ./calculator
   ```

## Continuous Integration

The test runner returns appropriate exit codes for CI/CD integration:
- Exit code `0`: All tests passed
- Exit code `1`: Some tests failed or error occurred

Example GitHub Actions usage:

```yaml
- name: Build Compiler
  run: |
    mkdir build
    cd build
    cmake ..
    make

- name: Run Tests
  run: ./run_tests.sh
```

## Language Features

### Data Types
- `int`: 32-bit signed integer
- `char`: 8-bit signed character (ASCII)
- `byte`: 8-bit unsigned integer (0-255)
- `bit`: Boolean (0 or 1)
- `float`: 32-bit floating point
- `double`: 64-bit floating point
- `string`: String pointer type
- Arrays: `var[10] arr:int` (static size)
- Structs: Custom data types with fields and methods
- Pointers: `var p:*int` (heap memory)

### Operators
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Comparison: `==`, `!=`, `<`, `>`, `<=`, `>=`
- Logical: `&&`, `||`, `!`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`
- Increment/Decrement: `++`, `--`

### Control Flow
- `if` / `else if` / `else`
- `for` loops
- Function calls

## Contributing

When contributing to the compiler:

1. Add tests for new features in `tests/`
2. Ensure all existing tests still pass
3. Update documentation as needed
4. Follow the existing code style

## License

[Add license information here]

## Authors

- Philipp01105

## Version

Current version: 5.1.0 (Struct Edition)

## Support

For issues, questions, or contributions, please use the GitHub issue tracker.
