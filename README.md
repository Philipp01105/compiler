# DMM Compiler

A complete compiler for the DMM programming language that compiles to x86-64 assembly and native executables.

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen.svg)]()
[![Language](https://img.shields.io/badge/language-C-blue.svg)]()
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)]()
[![License](https://img.shields.io/badge/license-MIT-blue.svg)]()

## 🚀 Features

- **7 Data Types**: int, char, byte, bit, string, arrays, structs with methods
- **Control Flow**: if/else, else-if chains, for loops, nested structures
- **Functions**: User-defined functions with parameters and return values
- **Object-Oriented**: Structs with fields and methods for encapsulation
- **Memory Management**: Pointers, heap allocation (reserve), deallocation (free), garbage collection
- **I/O System**: Built-in print/println functions with string concatenation
- **Operators**: Full arithmetic, comparison, logical, and assignment operators
- **Native Code**: Compiles to x86-64 assembly (Intel and AT&T syntax)
- **Cross-Platform**: Supports both ELF (Linux) and COFF (Windows) formats

## 📋 Table of Contents

- [Quick Start](#-quick-start)
- [Installation](#-installation)
- [Usage](#-usage)
- [Language Reference](#-language-reference)
- [Examples](#-examples)
- [Testing](#-testing)
- [Project Structure](#-project-structure)
- [Contributing](#-contributing)
- [License](#-license)

## ⚡ Quick Start

```bash
# Build the compiler
mkdir build && cd build
cmake ..
make

# Compile a DMM program
./compiler examples/hello.dmm

# Assemble and run
gcc -no-pie examples/hello.dmm.s -o hello
./hello
```

## 💾 Installation

### Prerequisites

- **CMake** 3.10 or higher
- **GCC** or compatible C compiler supporting C23
- **Make** build system
- **GCC Assembler** (for linking compiled programs)

### Windows (MinGW-w64)

```bash
# Install MinGW-w64 and CMake
# Build the compiler
mkdir build
cd build
cmake -G "MinGW Makefiles" ..
mingw32-make
```

### Linux

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get install build-essential cmake gcc

# Build the compiler
mkdir build
cd build
cmake ..
make
```

### Build Output

The compiler executable will be created as:
- `build/compiler` (Linux)
- `build/compiler.exe` (Windows)

## 🔧 Usage

### Basic Compilation

```bash
# Compile a DMM source file
./compiler source.dmm

# This generates source.dmm.s (assembly file)
```

### Compiler Options

```bash
./compiler [OPTIONS] <source_file>

Options:
  --tokens       Show generated token stream
  --debug        Enable debug output during parsing
  --formatError  Output errors in JSON format
  --syntax=MODE  Assembly syntax: att or intel (default: intel)
  --target=FMT   Target format: elf or coff (default: auto-detect)
  --help         Show help message
```

### Complete Workflow

```bash
# 1. Compile DMM to assembly
./compiler program.dmm

# 2. Assemble to executable
gcc -no-pie program.dmm.s -o program

# 3. Run the program
./program
```

### Example Commands

```bash
# Show token stream for debugging
./compiler --tokens program.dmm

# Enable debug mode with AT&T syntax
./compiler --debug --syntax=att program.dmm

# Compile for specific target format
./compiler --target=elf program.dmm     # Linux ELF format
./compiler --target=coff program.dmm    # Windows COFF format
```

## 📖 Language Reference

### Data Types

| Type | Size | Range | Description |
|------|------|-------|-------------|
| `int` | 4 bytes | -2³¹ to 2³¹-1 | 32-bit signed integer |
| `char` | 1 byte | -128 to 127 | 8-bit signed character (ASCII) |
| `byte` | 1 byte | 0 to 255 | 8-bit unsigned integer |
| `bit` | 1 byte | 0 or 1 | Boolean value |
| `string` | 8 bytes | Pointer | String literal pointer |
| `*type` | 8 bytes | Pointer | Pointer to any type |
| `struct` | Variable | N/A | Custom data structures |
| `[n]type` | Variable | N/A | Static arrays |

### Operators

**Arithmetic**: `+` `-` `*` `/` `%`  
**Comparison**: `==` `!=` `<` `>` `<=` `>=`  
**Logical**: `&&` `||` `!`  
**Assignment**: `=` `+=` `-=` `*=` `/=`  
**Increment/Decrement**: `++` `--`

### Control Structures

```javascript
// If-Else
if (condition) {
    // code
} else if (condition) {
    // code
} else {
    // code
}

// For Loop
for (var i:int = 0; i < 10; i++) {
    // code
}
```

### Functions

```javascript
func functionName(param1:type, param2:type) -> returnType {
    // code
    return value;
}
```

### Structs with Methods

```javascript
struct StructName {
    var field1:int;
    var field2:string;
    
    func method(param:type) -> returnType {
        // Access fields directly
        field1 = param;
        return field2;
    }
}
```

### Arrays

```javascript
// Declaration
var[10] numbers:int;

// Access and assignment
numbers[0] = 42;
var x:int = numbers[0];
```

### Pointers and Heap Management

```javascript
// Reserve heap memory
var ptr:*int = reserve int;

// Assign value
*ptr = 100;

// Free memory
free ptr;

// Garbage collection (automatic cleanup)
// Returns from functions trigger GC for local pointers
```

### Built-in Functions

```javascript
// Output without newline
print("Hello");

// Output with newline
println("World");

// String concatenation
println("Value: " + 42 + ", Name: " + name);
```

## 🎯 Examples

### Hello World

```javascript
func main() -> void {
    println("Hello, World!");
}
```

### Variables and Operations

```javascript
func main() -> void {
    var x:int = 10;
    var y:int = 20;
    var sum:int = x + y;
    println("Sum: " + sum);
}
```

### Functions with Recursion

```javascript
func factorial(n:int) -> int {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

func main() -> void {
    println("5! = " + factorial(5));
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
    
    func display() -> void {
        println("Point(" + x + ", " + y + ")");
    }
}

func main() -> void {
    var p:Point;
    p.x = 10;
    p.y = 20;
    p.display();
    p.move(5, 3);
    p.display();
}
```

### More Examples

- **Basic Examples**: [code_examples/](code_examples/) directory contains 12+ comprehensive examples
- **Language Documentation**: [examples/LANGUAGE_DOCUMENTATION.md](examples/LANGUAGE_DOCUMENTATION.md)
- **Test Suite**: [tests/](tests/) directory with 20+ test programs
- **Game Examples**: Calculator, Tic-Tac-Toe, Rock-Paper-Scissors, and more

## 🧪 Testing

### Automated Test Suite

The project includes a comprehensive testing system:

```bash
# Run all tests
./run_tests.sh

# Test output shows compilation, execution, and validation
```

### Test Results Format

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

Total Tests:  20
Passed:       20
Failed:       0

✓ ALL TESTS PASSED
```

### Test Categories

- **Basic Tests**: hello, variables, arithmetic
- **Control Flow**: conditionals, loops, nested structures
- **Functions**: parameters, return values, recursion
- **Strings**: operations, comparison, concatenation
- **Structs**: basic, methods, nested structures
- **Heap Management**: pointers, reserve, free, garbage collection
- **I/O System**: syscalls, complete I/O demonstrations

### Adding New Tests

```bash
# 1. Create test file
nano tests/my_test.dmm

# 2. Run test to generate output
./run_tests.sh

# 3. Save expected output
cp test_output/my_test.out tests/expected/my_test.expected

# 4. Verify test passes
./run_tests.sh
```

## 📁 Project Structure

```
compiler/
├── src/                        # Compiler source code
│   ├── main.c                  # Entry point and CLI
│   ├── lexer.c/h               # Lexical analysis (tokenization)
│   ├── parser_core.c           # Parser initialization and utilities
│   ├── parser_types.c          # Type system and declarations
│   ├── parser_expressions.c    # Expression parsing and evaluation
│   ├── parser_statements.c     # Statement parsing (if/for/etc.)
│   ├── parser_declarations.c   # Function and struct declarations
│   ├── parser_codegen.c        # Code generation utilities
│   ├── parser.h                # Parser public interface
│   ├── parser_internal.h       # Parser internal definitions
│   ├── compiler_types.h        # Type definitions
│   ├── errorHandler.c/h        # Error reporting system
│   ├── asm_optimizer.c/h       # Assembly optimization
│   ├── instruction_builder.c/h # Instruction generation
│   └── syntax_converter.c/h    # Syntax conversion (AT&T/Intel)
│
├── tests/                      # Test suite
│   ├── *.dmm                   # Test programs
│   ├── expected/               # Expected test outputs
│   └── README.md               # Testing documentation
│
├── code_examples/              # Example programs
│   ├── calculator.dmm          # Calculator with memory
│   ├── tictactoe.dmm           # Tic-Tac-Toe game
│   ├── shapes.dmm              # Nested structs example
│   ├── heap_management_demo.dmm # Pointer and heap demo
│   └── README.md               # Examples documentation
│
├── examples/                   # Documentation and guides
│   ├── LANGUAGE_DOCUMENTATION.md  # Complete language reference
│   ├── IO_SYSTEM_DOCUMENTATION.md # I/O system details
│   ├── HEAP_MANAGEMENT_DOCUMENTATION.md # Memory management
│   ├── STRUCT_FEATURE_DOCUMENTATION.md  # Struct features
│   └── README.md               # Additional documentation
│
├── stdlib/                     # Standard library (future)
├── build/                      # Build output (generated)
├── CMakeLists.txt              # CMake build configuration
├── run_tests.sh                # Automated test runner
├── validate_tests.sh           # Test validation script
└── README.md                   # This file
```

## 🏗️ Architecture

### Compiler Pipeline

```
Source Code (.dmm)
    ↓
┌─────────────┐
│   LEXER     │  Tokenization
│ (lexer.c)   │  → Token stream
└─────────────┘
    ↓
┌─────────────┐
│   PARSER    │  Syntax Analysis
│ (parser_*.c)│  → AST (implicit)
└─────────────┘
    ↓
┌─────────────┐
│  CODE GEN   │  Assembly Generation
│ (codegen.c) │  → .s file
└─────────────┘
    ↓
┌─────────────┐
│ ASSEMBLER   │  Machine Code
│    (gcc)    │  → Executable
└─────────────┘
```

### Key Components

1. **Lexer** (`lexer.c`): Tokenizes source code into meaningful symbols
2. **Parser** (`parser_*.c`): Implements recursive descent parsing with operator precedence
3. **Code Generator** (`parser_codegen.c`): Emits x86-64 assembly instructions
4. **Error Handler** (`errorHandler.c`): Provides detailed error messages with line/column info
5. **Type System** (`compiler_types.h`): Manages data types and type checking
6. **Optimizer** (`asm_optimizer.c`): Performs peephole optimizations on assembly

## 🛠️ Development

### Building from Source

```bash
# Clone the repository
git clone <repository-url>
cd compiler

# Create build directory
mkdir build && cd build

# Configure and build
cmake ..
make

# Run tests
cd ..
./run_tests.sh
```

### Debugging the Compiler

```bash
# Show tokens during compilation
./compiler --tokens program.dmm

# Enable verbose debug output
./compiler --debug program.dmm

# Format errors as JSON
./compiler --formatError program.dmm
```

### Code Style

- Written in C23 standard
- Modular architecture with clear separation of concerns
- Comprehensive error handling
- Extensive inline comments for complex logic

## 🤝 Contributing

Contributions are welcome! Here's how to contribute:

1. **Fork the repository**
2. **Create a feature branch**: `git checkout -b feature/new-feature`
3. **Write tests** for new features
4. **Ensure all tests pass**: `./run_tests.sh`
5. **Commit changes**: `git commit -am 'Add new feature'`
6. **Push to branch**: `git push origin feature/new-feature`
7. **Submit a pull request**

### Contribution Guidelines

- Add tests for new features in `tests/`
- Update documentation as needed
- Follow existing code style
- Ensure all existing tests still pass
- Add examples for significant features

## 📝 Documentation

- **[Language Documentation](examples/LANGUAGE_DOCUMENTATION.md)**: Complete language reference with examples
- **[Test Documentation](tests/README.md)**: Testing system and procedures
- **[Code Examples](code_examples/README.md)**: Comprehensive examples catalog
- **[I/O System](examples/IO_SYSTEM_DOCUMENTATION.md)**: Input/output system details
- **[Heap Management](examples/HEAP_MANAGEMENT_DOCUMENTATION.md)**: Memory management guide
- **[Struct Features](examples/STRUCT_FEATURE_DOCUMENTATION.md)**: Object-oriented programming

## 🎓 Learning Resources

### For Beginners

1. Start with basic examples in `code_examples/`
2. Read [LANGUAGE_DOCUMENTATION.md](examples/LANGUAGE_DOCUMENTATION.md)
3. Try modifying existing examples
4. Write simple programs and compile them

### For Advanced Users

1. Study the compiler source code in `src/`
2. Read [BACKEND_REWRITE_PLAN.md](BACKEND_REWRITE_PLAN.md) for architecture details
3. Explore complex examples like `tictactoe.dmm`
4. Contribute to the compiler or standard library

## 🐛 Known Limitations

- Floating-point support is implemented but limited in testing
- No dynamic arrays (only static sized arrays)
- Standard library is minimal (future expansion planned)
- Limited string manipulation functions
- No preprocessor or macro system

## 🗺️ Roadmap

### Planned Features

- [ ] Expanded standard library
- [ ] Dynamic array support
- [ ] More string operations
- [ ] Module/import system
- [ ] Optimization improvements
- [ ] Enhanced error messages
- [ ] IDE integration support

### Long-term Goals

- [ ] Self-hosting compiler (written in DMM)
- [ ] LLVM backend
- [ ] Additional target architectures
- [ ] Package manager
- [ ] Comprehensive standard library

## 📜 License

This project is licensed under the MIT License - see the LICENSE file for details.

## 👤 Author

**Philipp01105**

## 🙏 Acknowledgments

- Inspired by classic compiler design principles
- Built with educational purposes in mind
- Community contributions and feedback

## 📞 Support

For questions, issues, or contributions:

- **Issues**: Use the GitHub issue tracker
- **Documentation**: See the `examples/` directory
- **Examples**: Check `code_examples/` for working code

---

**Version**: 5.1.0 (Struct Edition)  
**Last Updated**: 2025-12-27  
**Status**: ✅ Production Ready
